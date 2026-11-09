#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/dml/ob_ai_column_runtime.h"

#include <atomic>
#include <stdio.h>

#include "lib/ob_define.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/ob_print_utils.h"
#include "share/ob_errno.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr.h"

namespace oceanbase
{
namespace sql
{
using common::ObString;
using common::ObMySQLProxy;
namespace
{

static const char *AI_TASK_TABLE_NAME = "oceanbase.__all_ai_column_task";
// The three hidden columns that back an AI column are named with this prefix,
// so the schema alone tells whether a table has one.
static const char *AI_HIDDEN_PREFIX = "__ai_";

// One row of oceanbase.__all_ai_column, in the shape the DML path needs.
struct AiColumnMeta
{
  AiColumnMeta()
    : column_id_(OB_INVALID_ID),
      state_column_id_(OB_INVALID_ID),
      request_column_id_(OB_INVALID_ID),
      max_retries_(0)
  {}
  uint64_t column_id_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  int64_t max_retries_;
  ObString column_name_;
  ObString dep_column_ids_;
  ObString pk_column_ids_;
  ObString pk_column_names_;
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

// Reads the AI column definitions of one table through the nested session the
// caller has already opened.
int load_ai_columns(ObTableModifyOp &op,
                    const uint64_t table_id,
                    common::ObIAllocator &alloc,
                    common::ObIArray<AiColumnMeta> &metas)
{
  int ret = OB_SUCCESS;
  char sql[512];
  snprintf(sql, sizeof(sql),
           "select column_id, column_name, dep_column_ids, pk_column_ids, "
           "pk_column_names, max_retries, state_column_id, request_column_id "
           "from oceanbase.__all_ai_column where table_id = %lu",
           static_cast<unsigned long>(table_id));
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    sqlclient::ObMySQLResult *result = NULL;
    if (OB_FAIL(op.execute_read(sql, res))) {
      // The metadata table may not exist yet while the system is bootstrapping.
      if (OB_TABLE_NOT_EXIST == ret) {
        LOG_INFO("ai column metadata is not ready, skip scheduling", K(table_id));
        ret = OB_SUCCESS;
      }
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        AiColumnMeta meta;
        ObString column_name;
        ObString dep_column_ids;
        ObString pk_column_ids;
        ObString pk_column_names;
        if (OB_FAIL(result->get_uint(static_cast<int64_t>(0), meta.column_id_))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(1), column_name))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(2), dep_column_ids))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(3), pk_column_ids))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(4), pk_column_names))) {
        } else if (OB_FAIL(result->get_int(static_cast<int64_t>(5), meta.max_retries_))) {
        } else if (OB_FAIL(result->get_uint(static_cast<int64_t>(6), meta.state_column_id_))) {
        } else if (OB_FAIL(result->get_uint(static_cast<int64_t>(7), meta.request_column_id_))) {
        } else if (OB_FAIL(ob_write_string(alloc, column_name, meta.column_name_))) {
        } else if (OB_FAIL(ob_write_string(alloc, dep_column_ids, meta.dep_column_ids_))) {
        } else if (OB_FAIL(ob_write_string(alloc, pk_column_ids, meta.pk_column_ids_))) {
        } else if (OB_FAIL(ob_write_string(alloc, pk_column_names, meta.pk_column_names_))) {
        } else if (OB_FAIL(metas.push_back(meta))) {
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
    if (OB_NOT_NULL(result)) {
      const int close_ret = result->close();
      if (OB_SUCCESS != close_ret && OB_SUCC(ret)) {
        ret = close_ret;
      }
    }
  }
  return ret;
}

// Builds the rowkey predicate of the modified row, e.g. "`id` = 2".
int build_rowkey_predicate(ObTableModifyOp &op,
                           const ObDMLBaseCtDef &ctdef,
                           const AiColumnMeta &meta,
                           const ExprFixedArray &row,
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
    if (OB_FAIL(ForeignKeyHandle::gen_row_predicate(op.get_eval_ctx(), where_buf, where_len,
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

// True when any column of the dependency list carries a different value in the
// old row and the new row.
int dependency_changed(ObTableModifyOp &op,
                       const ObDMLBaseCtDef &ctdef,
                       const AiColumnMeta &meta,
                       bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  ObSEArray<uint64_t, 8> dep_ids;
  if (OB_FAIL(split_column_ids(meta.dep_column_ids_, dep_ids))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && !changed && i < dep_ids.count(); ++i) {
      int64_t idx = -1;
      for (int64_t k = 0; k < ctdef.column_ids_.count(); ++k) {
        if (ctdef.column_ids_.at(k) == dep_ids.at(i)) {
          idx = k;
          break;
        }
      }
      if (idx < 0) {
        // The column is not read by this statement, so its value cannot change.
        continue;
      } else if (idx >= ctdef.old_row_.count() || idx >= ctdef.new_row_.count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("dependency column is out of the row", K(ret), K(idx));
      } else {
        ObDatum *old_datum = NULL;
        ObDatum *new_datum = NULL;
        if (OB_FAIL(ctdef.old_row_.at(idx)->eval(op.get_eval_ctx(), old_datum))) {
        } else if (OB_FAIL(ctdef.new_row_.at(idx)->eval(op.get_eval_ctx(), new_datum))) {
        } else if (OB_ISNULL(old_datum) || OB_ISNULL(new_datum)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (0 != old_datum->compare(*new_datum,
                                           ctdef.old_row_.at(idx)->datum_meta_,
                                           ctdef.new_row_.at(idx)->datum_meta_)) {
          changed = true;
        }
      }
    }
  }
  return ret;
}

} // namespace

int ObAiColumnRuntime::has_ai_column(ObTableModifyOp &op,
                                     const uint64_t table_id,
                                     bool &has_ai_column)
{
  int ret = OB_SUCCESS;
  has_ai_column = false;
  share::schema::ObSchemaGetterGuard *schema_guard = NULL;
  const share::schema::ObTableSchema *table_schema = NULL;
  ObSqlCtx *sql_ctx = op.get_exec_ctx().get_sql_ctx();
  if (OB_ISNULL(sql_ctx) || OB_ISNULL(schema_guard = sql_ctx->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema guard is not available", K(ret));
  } else if (OB_FAIL(schema_guard->get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const int64_t column_count = table_schema->get_column_count();
    for (int64_t i = 0; OB_SUCC(ret) && !has_ai_column && i < column_count; ++i) {
      const share::schema::ObColumnSchemaV2 *column = table_schema->get_column_schema_by_idx(i);
      if (OB_ISNULL(column)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (column->is_hidden()) {
        const ObString name = column->get_column_name_str();
        if (name.length() > static_cast<int32_t>(STRLEN(AI_HIDDEN_PREFIX))
            && 0 == name.compare(ObString::make_string(AI_HIDDEN_PREFIX),
                                 static_cast<int32_t>(STRLEN(AI_HIDDEN_PREFIX)))) {
          has_ai_column = true;
        }
      }
    }
  }
  return ret;
}

int ObAiColumnRuntime::schedule_row(ObTableModifyOp &op,
                                    const ObDMLBaseCtDef &ctdef,
                                    const int64_t dml_event)
{
  int ret = OB_SUCCESS;
  const uint64_t table_id = ctdef.das_base_ctdef_.index_tid_;
  const ObDmlEventType event = static_cast<ObDmlEventType>(dml_event);
  const ExprFixedArray *row = NULL;
  bool need_schedule = false;
  if (is_inner_table(table_id)) {
    // AI columns only exist on user tables, and bootstrap inserts into the
    // system tables before __all_ai_column is created.
  } else if (OB_FAIL(has_ai_column(op, table_id, need_schedule))) {
  } else if (!need_schedule) {
  } else if (ObDmlEventType::DE_DELETING == event) {
    row = &ctdef.old_row_;
  } else {
    row = &ctdef.new_row_;
  }

  if (OB_SUCC(ret) && need_schedule && OB_NOT_NULL(row)) {
    common::ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR, OB_MALLOC_NORMAL_BLOCK_SIZE);
    ObSEArray<AiColumnMeta, 8> metas;
    bool started = false;
    if (OB_FAIL(op.ensure_inner_conn())) {
    } else if (OB_FAIL(op.begin_nested_session(false))) {
    } else if (FALSE_IT(started = true)) {
    } else if (OB_FAIL(load_ai_columns(op, table_id, alloc, metas))) {
    } else if (metas.empty()) {
    } else {
      ObString rowkey_predicate;
      static std::atomic<int64_t> task_seq(0);
      const int64_t seq = task_seq.fetch_add(1, std::memory_order_relaxed);
      char request_id[64];
      snprintf(request_id, sizeof(request_id), "%ld-%ld",
               ObTimeUtility::current_time(), static_cast<long>(seq));
      if (OB_FAIL(build_rowkey_predicate(op, ctdef, metas.at(0), *row, alloc,
                                         rowkey_predicate))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
        const AiColumnMeta &meta = metas.at(i);
        bool changed = true;
        char sql[2048];
        if (ObDmlEventType::DE_UPDATING == event && OB_FAIL(dependency_changed(op, ctdef, meta, changed))) {
        } else if (!changed) {
          // An unrelated update must not invalidate the AI column.
          continue;
        }
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
    if (started) {
      const int end_ret = op.end_nested_session();
      if (OB_SUCCESS != end_ret && OB_SUCC(ret)) {
        ret = end_ret;
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
