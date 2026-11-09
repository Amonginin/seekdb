#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/dml/ob_ai_column_runtime.h"

#include <atomic>
#include <stdio.h>

#include "lib/time/ob_time_utility.h"
#include "lib/utility/ob_print_utils.h"
#include "share/ob_errno.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr.h"

namespace oceanbase
{
namespace sql
{
namespace
{

static const char *AI_TASK_TABLE_NAME = "oceanbase.__all_ai_column_task";

// One row of oceanbase.__all_ai_column, in the shape the DML path needs.
struct AiColumnMeta
{
  AiColumnMeta()
    : column_id_(OB_INVALID_ID),
      state_column_id_(OB_INVALID_ID),
      request_column_id_(OB_INVALID_ID),
      max_retries_(0),
      result_type_(0)
  {}
  uint64_t column_id_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  int64_t max_retries_;
  int64_t result_type_;
  ObString column_name_;
  ObString pk_column_ids_;
  ObString pk_column_names_;
  ObString sync_mode_;
};

// Parses the comma separated column id list persisted in the metadata.
int split_column_ids(const ObString &text, common::ObIArray<uint64_t> &ids)
{
  int ret = OB_SUCCESS;
  int64_t start = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i <= text.length(); ++i) {
    if (i == text.length() || ',' == text.ptr()[i]) {
      if (i > start) {
        int64_t value = 0;
        bool valid = true;
        for (int64_t k = start; valid && k < i; ++k) {
          const char c = text.ptr()[k];
          if (c < '0' || c > '9') {
            valid = false;
          } else {
            value = value * 10 + (c - '0');
          }
        }
        if (valid && OB_FAIL(ids.push_back(static_cast<uint64_t>(value)))) {
        }
      }
      start = i + 1;
    }
  }
  return ret;
}

// Parses the comma separated column name list persisted in the metadata.
int split_column_names(common::ObIAllocator &alloc,
                       const ObString &text,
                       common::ObIArray<ObString> &names)
{
  int ret = OB_SUCCESS;
  int64_t start = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i <= text.length(); ++i) {
    if (i == text.length() || ',' == text.ptr()[i]) {
      if (i > start) {
        ObString name;
        if (OB_FAIL(ob_write_string(alloc, ObString(i - start, text.ptr() + start), name))) {
        } else if (OB_FAIL(names.push_back(name))) {
        }
      }
      start = i + 1;
    }
  }
  return ret;
}

// Reads the AI column definitions of one table. The metadata lives in an inner
// table, so the read goes through a nested session and shares the transaction
// of the running statement.
int load_ai_columns(ObTableModifyOp &op,
                    const uint64_t table_id,
                    common::ObIAllocator &alloc,
                    common::ObIArray<AiColumnMeta> &metas)
{
  int ret = OB_SUCCESS;
  char sql[512];
  snprintf(sql, sizeof(sql),
           "select column_id, column_name, pk_column_ids, pk_column_names, sync_mode, "
           "max_retries, result_type, state_column_id, request_column_id "
           "from oceanbase.__all_ai_column where table_id = %lu",
           static_cast<unsigned long>(table_id));
  if (OB_FAIL(op.open_inner_conn())) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::ObMySQLResult *result = NULL;
      bool started = false;
      if (OB_FAIL(op.begin_nested_session(false))) {
      } else {
        started = true;
        if (OB_FAIL(op.execute_read(sql, res))) {
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          while (OB_SUCC(ret) && OB_SUCC(result->next())) {
            AiColumnMeta meta;
            ObString column_name;
            ObString pk_column_ids;
            ObString pk_column_names;
            ObString sync_mode;
            if (OB_FAIL(result->get_uint(0, meta.column_id_))) {
            } else if (OB_FAIL(result->get_varchar(1, column_name))) {
            } else if (OB_FAIL(result->get_varchar(2, pk_column_ids))) {
            } else if (OB_FAIL(result->get_varchar(3, pk_column_names))) {
            } else if (OB_FAIL(result->get_varchar(4, sync_mode))) {
            } else if (OB_FAIL(result->get_int(5, meta.max_retries_))) {
            } else if (OB_FAIL(result->get_int(6, meta.result_type_))) {
            } else if (OB_FAIL(result->get_uint(7, meta.state_column_id_))) {
            } else if (OB_FAIL(result->get_uint(8, meta.request_column_id_))) {
            } else if (OB_FAIL(ob_write_string(alloc, column_name, meta.column_name_))) {
            } else if (OB_FAIL(ob_write_string(alloc, pk_column_ids, meta.pk_column_ids_))) {
            } else if (OB_FAIL(ob_write_string(alloc, pk_column_names, meta.pk_column_names_))) {
            } else if (OB_FAIL(ob_write_string(alloc, sync_mode, meta.sync_mode_))) {
            } else if (OB_FAIL(metas.push_back(meta))) {
            }
          }
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
          }
        }
        int close_ret = OB_SUCCESS;
        if (OB_NOT_NULL(result)) {
          close_ret = result->close();
          if (OB_SUCCESS != close_ret && OB_SUCC(ret)) {
            ret = close_ret;
          }
        }
        if (started) {
          int end_ret = op.end_nested_session();
          if (OB_SUCCESS != end_ret && OB_SUCC(ret)) {
            ret = end_ret;
          }
        }
      }
    }
  }
  return ret;
}

// Builds the rowkey predicate of the modified row, e.g. "`id` = 2".
int build_rowkey_predicate(ObTableModifyOp &op,
                           const ObDMLBaseCtDef &ctdef,
                           const AiColumnMeta &meta,
                           const common::ObIArray<ObExpr *> &row,
                           common::ObIAllocator &alloc,
                           ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSEArray<uint64_t, 8> pk_ids;
  ObSEArray<ObString, 8> pk_names;
  ObSEArray<ObForeignKeyColumn, 8> pk_columns;
  char *where_buf = NULL;
  int64_t where_len = 0;
  int64_t where_pos = 0;
  if (OB_FAIL(split_column_ids(meta.pk_column_ids_, pk_ids))) {
  } else if (OB_FAIL(split_column_names(alloc, meta.pk_column_names_, pk_names))) {
  } else if (pk_ids.empty() || pk_ids.count() != pk_names.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected rowkey metadata of the ai column", K(ret),
             K(pk_ids.count()), K(pk_names.count()));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < pk_ids.count(); ++i) {
      int64_t idx = -1;
      for (int64_t k = 0; k < ctdef.column_ids_.count(); ++k) {
        if (ctdef.column_ids_.at(k) == pk_ids.at(i)) {
          idx = k;
          break;
        }
      }
      if (idx < 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("rowkey column is not part of the modified row", K(ret),
                 K(pk_ids.at(i)), K(ctdef.column_ids_.count()));
      } else {
        ObForeignKeyColumn column;
        column.name_ = pk_names.at(i);
        column.idx_ = static_cast<int32_t>(idx);
        if (OB_FAIL(pk_columns.push_back(column))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ForeignKeyHandle::gen_where(op.get_eval_ctx(), where_buf, where_len,
                                            where_pos, pk_columns, row, alloc,
                                            op.get_obj_print_params()))) {
    } else if (OB_ISNULL(where_buf) || where_pos <= 1) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      // gen_column_value() appends a terminator, so the text is one byte shorter.
      predicate.assign_ptr(where_buf, static_cast<int32_t>(where_pos - 1));
    }
  }
  return ret;
}

} // namespace

int ObAiColumnRuntime::create_tasks_for_insert(ObTableModifyOp &op,
                                               const ObDMLBaseCtDef &ctdef,
                                               const common::ObIArray<ObExpr *> &row,
                                               common::ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  const uint64_t table_id = ctdef.das_base_ctdef_.index_tid_;
  ObSEArray<AiColumnMeta, 8> metas;
  if (OB_FAIL(load_ai_columns(op, table_id, alloc, metas))) {
  } else if (metas.empty()) {
    // The table has no AI column, nothing to schedule.
  } else {
    ObString rowkey_predicate;
    if (OB_FAIL(build_rowkey_predicate(op, ctdef, metas.at(0), row, alloc, rowkey_predicate))) {
    } else {
      static std::atomic<int64_t> task_seq(0);
      int64_t seq = task_seq.fetch_add(1, std::memory_order_relaxed);
      char request_id[64];
      snprintf(request_id, sizeof(request_id), "%ld-%ld",
               ObTimeUtility::current_time(), static_cast<long>(seq));
      for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
        const AiColumnMeta &meta = metas.at(i);
        char sql[2048];
        snprintf(sql, sizeof(sql),
                 "insert into %s (table_id, request_id, column_id, column_name, "
                 "rowkey_predicate, state, retry_count, eligible_ts) "
                 "values (%lu, '%s', %lu, '%.*s', '%.*s', 0, 0, now())",
                 AI_TASK_TABLE_NAME,
                 static_cast<unsigned long>(table_id),
                 request_id,
                 static_cast<unsigned long>(meta.column_id_),
                 meta.column_name_.length(), meta.column_name_.ptr(),
                 rowkey_predicate.length(), rowkey_predicate.ptr());
        if (OB_FAIL(op.execute_write(sql))) {
          LOG_WARN("failed to create the ai column task", K(ret), K(table_id),
                   K(meta.column_id_));
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
