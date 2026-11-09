#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/dml/ob_ai_column_runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "common/object/ob_object.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/ob_print_utils.h"
#include "share/ob_errno.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_table_schema.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr.h"
#include "sql/ob_sql_context.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace
{

static const char *AI_TASK_TABLE_NAME = "oceanbase.__all_ai_column_task";

void ai_trace_impl(const char *fmt, ...)
{
  const int fd = ::open("/tmp/aidbg.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    const int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
      const int to_write = len < static_cast<int>(sizeof(buf)) ? len : static_cast<int>(sizeof(buf));
      const ssize_t ignored = ::write(fd, buf, static_cast<size_t>(to_write));
      UNUSED(ignored);
    }
    ::close(fd);
  }
}

// One row of oceanbase.__all_ai_column, in the shape the flush needs.
struct AiColumnMeta
{
  AiColumnMeta()
    : column_id_(OB_INVALID_ID),
      max_retries_(0)
  {}
  uint64_t column_id_;
  int64_t max_retries_;
  ObString column_name_;
  ObString dep_column_ids_;
};

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

// Builds the rowkey predicate of a row, e.g. "`id` = 2". The column names come
// from the schema in memory, so no SQL is involved.
int build_rowkey_predicate(ObTableModifyOp &op,
                           const share::schema::ObTableSchema &table_schema,
                           const ObDMLBaseCtDef &ctdef,
                           const ExprFixedArray &row,
                           common::ObIAllocator &alloc,
                           ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSEArray<uint64_t, 8> rowkey_ids;
  ObSEArray<ObForeignKeyColumn, 8> rowkey_columns;
  char *where_buf = NULL;
  int64_t where_len = 0;
  int64_t where_pos = 0;
  if (OB_FAIL(table_schema.get_rowkey_info().get_column_ids(rowkey_ids))) {
  } else if (rowkey_ids.empty()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the table has no rowkey", K(ret), K(table_schema.get_table_id()));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_ids.count(); ++i) {
      int64_t idx = -1;
      for (int64_t k = 0; k < ctdef.column_ids_.count(); ++k) {
        if (ctdef.column_ids_.at(k) == rowkey_ids.at(i)) {
          idx = k;
          break;
        }
      }
      const share::schema::ObColumnSchemaV2 *column =
          table_schema.get_column_schema(rowkey_ids.at(i));
      if (idx < 0 || OB_ISNULL(column)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("a rowkey column is not part of the modified row", K(ret),
                 K(rowkey_ids.at(i)), K(ctdef.column_ids_.count()));
      } else {
        ObForeignKeyColumn key_column;
        key_column.name_ = column->get_column_name_str();
        key_column.idx_ = static_cast<int32_t>(idx);
        if (OB_FAIL(rowkey_columns.push_back(key_column))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ForeignKeyHandle::gen_row_predicate(op.get_eval_ctx(), where_buf, where_len,
                                                    where_pos, rowkey_columns, row,
                                                    alloc, op.get_obj_print_params()))) {
    } else if (OB_ISNULL(where_buf) || where_pos <= 1) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      // gen_column_value() appends a terminator, so the text is one byte short.
      if (OB_FAIL(ob_write_string(alloc, ObString(static_cast<int32_t>(where_pos - 1), where_buf),
                                  predicate))) {
      }
    }
  }
  return ret;
}

// Collects the ids of the columns whose value really changed. An update that
// touches nothing the AI function reads must not invalidate the column.
int collect_changed_ids(ObTableModifyOp &op,
                        const ObDMLBaseCtDef &ctdef,
                        common::ObIAllocator &alloc,
                        ObString &changed_ids)
{
  int ret = OB_SUCCESS;
  char buf[512];
  int64_t pos = 0;
  bool first = true;
  ObEvalCtx &eval_ctx = op.get_eval_ctx();
  for (int64_t i = 0; OB_SUCC(ret) && i < ctdef.column_ids_.count(); ++i) {
    bool changed = true;
    if (i < ctdef.old_row_.count() && i < ctdef.new_row_.count()) {
      ObDatum *old_datum = NULL;
      ObDatum *new_datum = NULL;
      if (OB_FAIL(ctdef.old_row_.at(i)->eval(eval_ctx, old_datum))) {
      } else if (OB_FAIL(ctdef.new_row_.at(i)->eval(eval_ctx, new_datum))) {
      } else if (OB_NOT_NULL(old_datum) && OB_NOT_NULL(new_datum)) {
        changed = !ObDatum::binary_equal(*old_datum, *new_datum);
      }
    }
    if (OB_SUCC(ret) && changed) {
      const int64_t written = snprintf(buf + pos, sizeof(buf) - pos, "%s%lu",
                                       first ? "" : ",",
                                       static_cast<unsigned long>(ctdef.column_ids_.at(i)));
      if (OB_UNLIKELY(written <= 0 || pos + written >= static_cast<int64_t>(sizeof(buf)))) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        pos += written;
        first = false;
      }
    }
  }
  if (OB_SUCC(ret) && pos > 0) {
    ret = ob_write_string(alloc, ObString(static_cast<int32_t>(pos), buf), changed_ids);
  }
  return ret;
}

// True when the two comma separated id lists share at least one id.
bool ids_intersect(const ObString &left, const ObString &right)
{
  ObSEArray<uint64_t, 16> left_ids;
  ObSEArray<uint64_t, 16> right_ids;
  bool found = false;
  if (OB_SUCCESS == split_column_ids(left, left_ids)
      && OB_SUCCESS == split_column_ids(right, right_ids)) {
    for (int64_t i = 0; !found && i < left_ids.count(); ++i) {
      for (int64_t k = 0; !found && k < right_ids.count(); ++k) {
        found = (left_ids.at(i) == right_ids.at(k));
      }
    }
  }
  return found;
}

// Reads the AI column definitions of the given tables. The metadata lives in
// an inner table, so this needs a short nested session of its own.
int load_ai_columns(ObTableModifyOp &op,
                    const ObIArray<uint64_t> &table_ids,
                    common::ObIAllocator &alloc,
                    ObIArray<AiColumnMeta> &metas)
{
  int ret = OB_SUCCESS;
  char sql[4096];
  ai_trace_impl("[ai] load enter v3 tables=%ld\n", table_ids.count());
  int64_t pos = snprintf(sql, sizeof(sql),
                         "select column_id, column_name, dep_column_ids, max_retries "
                         "from oceanbase.__all_ai_column where table_id in (");
  if (OB_UNLIKELY(pos <= 0 || pos >= static_cast<int64_t>(sizeof(sql)))) {
    ret = OB_SIZE_OVERFLOW;
    ai_trace_impl("[ai] load build sql failed pos=%ld\n", static_cast<long>(pos));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
      const int64_t written = snprintf(sql + pos, sizeof(sql) - pos, "%s%lu",
                                       i == 0 ? "" : ",",
                                       static_cast<unsigned long>(table_ids.at(i)));
      if (OB_UNLIKELY(written <= 0 || pos + written >= static_cast<int64_t>(sizeof(sql)))) {
        ret = OB_SIZE_OVERFLOW;
        ai_trace_impl("[ai] load build table list failed pos=%ld\n", static_cast<long>(pos));
      } else {
        pos += written;
      }
    }
    if (OB_SUCC(ret)) {
      const int64_t written = snprintf(sql + pos, sizeof(sql) - pos, ")");
      if (OB_UNLIKELY(written <= 0 || pos + written >= static_cast<int64_t>(sizeof(sql)))) {
        ret = OB_SIZE_OVERFLOW;
        ai_trace_impl("[ai] load build tail failed pos=%ld\n", static_cast<long>(pos));
      } else {
        pos += written;
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(op.open_inner_conn())) {
    ai_trace_impl("[ai] load open_inner_conn ret=%d\n", ret);
  } else {
    SMART_VAR(common::ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      bool started = false;
      sql[pos] = '\0';
      if (OB_FAIL(op.begin_nested_session(false))) {
        ai_trace_impl("[ai] load begin_nested ret=%d\n", ret);
      } else {
        started = true;
        if (OB_FAIL(op.execute_read(sql, res))) {
          ai_trace_impl("[ai] load execute_read ret=%d sql=%s\n", ret, sql);
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          while (OB_SUCC(ret) && OB_SUCC(result->next())) {
            AiColumnMeta meta;
            ObObj id_obj;
            ObObj name_obj;
            ObObj dep_obj;
            ObObj retry_obj;
            // get_obj() reports a null value as success, while the typed
            // getters reject it. dep_column_ids is nullable by design.
            if (OB_FAIL(result->get_obj(static_cast<int64_t>(0), id_obj))) {
            } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(1), name_obj))) {
            } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(2), dep_obj))) {
            } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(3), retry_obj))) {
            } else if (OB_UNLIKELY(!id_obj.is_integer_type() || !name_obj.is_string_type())) {
              ret = OB_ERR_UNEXPECTED;
              ai_trace_impl("[ai] load unexpected column types\n");
            } else {
              int64_t id_value = 0;
              int64_t retry_value = 0;
              ObString name;
              ObString dep;
              if (OB_FAIL(id_obj.get_int(id_value))) {
              } else if (OB_FAIL(name_obj.get_string(name))) {
              } else if (OB_FAIL(ob_write_string(alloc, name, meta.column_name_))) {
              } else if (dep_obj.is_string_type()
                         && OB_FAIL(dep_obj.get_string(dep))) {
              } else if (OB_FAIL(ob_write_string(alloc, dep, meta.dep_column_ids_))) {
              } else if (retry_obj.is_integer_type() && OB_FAIL(retry_obj.get_int(retry_value))) {
              } else {
                meta.column_id_ = static_cast<uint64_t>(id_value);
                meta.max_retries_ = retry_value;
                if (OB_FAIL(metas.push_back(meta))) {
                }
              }
            }
          }
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
          } else if (OB_FAIL(ret)) {
            ai_trace_impl("[ai] load row loop ret=%d metas=%ld\n", ret, metas.count());
          }
        }
        int close_ret = OB_SUCCESS;
        if (OB_NOT_NULL(result)) {
          close_ret = result->close();
          if (OB_SUCCESS != close_ret) {
            ai_trace_impl("[ai] load result close ret=%d\n", close_ret);
            if (OB_SUCC(ret)) {
              ret = close_ret;
            }
          }
        }
        if (started) {
          int end_ret = op.end_nested_session();
          if (OB_SUCCESS != end_ret) {
            ai_trace_impl("[ai] load end_nested ret=%d\n", end_ret);
            if (OB_SUCC(ret)) {
              ret = end_ret;
            }
          }
        }
        if (OB_FAIL(ret) && metas.count() > 0) {
          // The rows are already copied out, so a failure while closing the
          // cursor must not throw away usable metadata.
          ai_trace_impl("[ai] load keeps %ld rows despite ret=%d\n", metas.count(), ret);
          ret = OB_SUCCESS;
        }
      }
    }
  }
  ai_trace_impl("[ai] load done v3 ret=%d metas=%ld\n", ret, metas.count());
  return ret;
}

// Writes the task rows of all recorded rows in one nested session, so they
// join the transaction of the statement.
int write_task_rows(ObTableModifyOp &op,
                    const ObAiPendingRowArray &pending,
                    const ObIArray<AiColumnMeta> &metas)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(op.open_inner_conn())) {
    ai_trace_impl("[ai] write open_inner_conn ret=%d\n", ret);
  } else if (OB_FAIL(op.begin_nested_session(false))) {
    ai_trace_impl("[ai] write begin_nested ret=%d\n", ret);
  } else {
    static std::atomic<int64_t> task_seq(0);
    const int64_t seq = task_seq.fetch_add(1, std::memory_order_relaxed);
    char request_id[64];
    snprintf(request_id, sizeof(request_id), "%ld-%ld",
             ObTimeUtility::current_time(), static_cast<long>(seq));
    for (int64_t i = 0; OB_SUCC(ret) && i < pending.count(); ++i) {
      const ObAiPendingRow &row = pending.at(i);
      const ObDmlEventType event = static_cast<ObDmlEventType>(row.dml_event_);
      const bool pk_moved = (0 != row.rowkey_predicate_.compare(row.old_rowkey_predicate_));
      for (int64_t k = 0; OB_SUCC(ret) && k < metas.count(); ++k) {
        const AiColumnMeta &meta = metas.at(k);
        char action[3072];
        if (ObDmlEventType::DE_UPDATING == event
            && !ids_intersect(row.changed_column_ids_, meta.dep_column_ids_)) {
          // Nothing the AI function reads has changed.
          continue;
        } else if (ObDmlEventType::DE_DELETING == event) {
          snprintf(action, sizeof(action),
                   "delete from %s where table_id = %lu and rowkey_predicate = '%.*s'",
                   AI_TASK_TABLE_NAME,
                   static_cast<unsigned long>(row.table_id_),
                   row.rowkey_predicate_.length(), row.rowkey_predicate_.ptr());
        } else if (ObDmlEventType::DE_UPDATING == event && pk_moved) {
          char drop_sql[1024];
          snprintf(drop_sql, sizeof(drop_sql),
                   "delete from %s where table_id = %lu and rowkey_predicate = '%.*s'",
                   AI_TASK_TABLE_NAME,
                   static_cast<unsigned long>(row.table_id_),
                   row.old_rowkey_predicate_.length(), row.old_rowkey_predicate_.ptr());
          if (OB_FAIL(op.execute_write(drop_sql))) {
          }
          if (OB_SUCC(ret)) {
            snprintf(action, sizeof(action),
                     "insert into %s (table_id, request_id, column_id, column_name, "
                     "rowkey_predicate, state, retry_count, eligible_ts) "
                     "values (%lu, '%s', %lu, '%.*s', '%.*s', 0, 0, now())",
                     AI_TASK_TABLE_NAME,
                     static_cast<unsigned long>(row.table_id_),
                     request_id,
                     static_cast<unsigned long>(meta.column_id_),
                     meta.column_name_.length(), meta.column_name_.ptr(),
                     row.rowkey_predicate_.length(), row.rowkey_predicate_.ptr());
          }
        } else if (ObDmlEventType::DE_UPDATING == event) {
          // Same row, new value: replace the task so a result that is still in
          // flight for the previous request cannot be published.
          char drop_sql[1024];
          snprintf(drop_sql, sizeof(drop_sql),
                   "delete from %s where table_id = %lu and rowkey_predicate = '%.*s' "
                   "and column_id = %lu",
                   AI_TASK_TABLE_NAME,
                   static_cast<unsigned long>(row.table_id_),
                   row.rowkey_predicate_.length(), row.rowkey_predicate_.ptr(),
                   static_cast<unsigned long>(meta.column_id_));
          if (OB_FAIL(op.execute_write(drop_sql))) {
          }
          if (OB_SUCC(ret)) {
            snprintf(action, sizeof(action),
                     "insert into %s (table_id, request_id, column_id, column_name, "
                     "rowkey_predicate, state, retry_count, eligible_ts) "
                     "values (%lu, '%s', %lu, '%.*s', '%.*s', 0, 0, now())",
                     AI_TASK_TABLE_NAME,
                     static_cast<unsigned long>(row.table_id_),
                     request_id,
                     static_cast<unsigned long>(meta.column_id_),
                     meta.column_name_.length(), meta.column_name_.ptr(),
                     row.rowkey_predicate_.length(), row.rowkey_predicate_.ptr());
          }
        } else {
          snprintf(action, sizeof(action),
                   "insert into %s (table_id, request_id, column_id, column_name, "
                   "rowkey_predicate, state, retry_count, eligible_ts) "
                   "values (%lu, '%s', %lu, '%.*s', '%.*s', 0, 0, now())",
                   AI_TASK_TABLE_NAME,
                   static_cast<unsigned long>(row.table_id_),
                   request_id,
                   static_cast<unsigned long>(meta.column_id_),
                   meta.column_name_.length(), meta.column_name_.ptr(),
                   row.rowkey_predicate_.length(), row.rowkey_predicate_.ptr());
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(op.execute_write(action))) {
          ai_trace_impl("[ai] flush write failed ret=%d sql=%s\n", ret, action);
        } else if (ObDmlEventType::DE_DELETING != event) {
          // Mark the row with the request that is now responsible for it. The
          // worker only publishes a result when the marker still matches, so a
          // result that arrives after a newer request cannot win.
          char req_sql[2048];
          snprintf(req_sql, sizeof(req_sql),
                   "update `%.*s`.`%.*s` set `__ai_req_%.*s` = '%s' where %.*s",
                   row.database_name_.length(), row.database_name_.ptr(),
                   row.table_name_.length(), row.table_name_.ptr(),
                   meta.column_name_.length(), meta.column_name_.ptr(),
                   request_id,
                   row.rowkey_predicate_.length(), row.rowkey_predicate_.ptr());
          if (OB_FAIL(op.execute_write(req_sql))) {
            ai_trace_impl("[ai] mark request failed ret=%d sql=%s\n", ret, req_sql);
          }
        }
      }
      if (OB_SUCC(ret)) {
        ai_trace_impl("[ai] flushed table_id=%lu pred=%.*s\n",
                      static_cast<unsigned long>(row.table_id_),
                      row.rowkey_predicate_.length(), row.rowkey_predicate_.ptr());
      }
    }
    int end_ret = op.end_nested_session();
    if (OB_SUCCESS != end_ret && OB_SUCC(ret)) {
      ret = end_ret;
    }
  }
  return ret;
}

} // namespace

int ObAiColumnRuntime::record_row(ObTableModifyOp &op,
                                  const ObDMLBaseCtDef &ctdef,
                                  const int64_t dml_event,
                                  common::ObIAllocator &alloc,
                                  ObAiPendingRowArray &pending)
{
  int ret = OB_SUCCESS;
  const uint64_t table_id = ctdef.das_base_ctdef_.index_tid_;
  const ObDmlEventType event = static_cast<ObDmlEventType>(dml_event);
  share::schema::ObSchemaGetterGuard *schema_guard = NULL;
  const share::schema::ObTableSchema *table_schema = NULL;
  ObSqlCtx *sql_ctx = op.get_exec_ctx().get_sql_ctx();
  if (is_sys_table(table_id)) {
    // AI columns only exist on user tables.
  } else if (OB_ISNULL(sql_ctx) || OB_ISNULL(schema_guard = sql_ctx->schema_guard_)) {
    // Without a schema guard the row key cannot be rendered. The AI column is
    // secondary to the statement, so skip it instead of failing the write.
    ai_trace_impl("[ai] record skipped, no schema guard table_id=%lu\n",
                  static_cast<unsigned long>(table_id));
  } else if (OB_FAIL(schema_guard->get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObAiPendingRow row;
    row.table_id_ = table_id;
    row.dml_event_ = dml_event;
    const ExprFixedArray &current = (ObDmlEventType::DE_DELETING == event)
                                        ? ctdef.old_row_ : ctdef.new_row_;
    const share::schema::ObDatabaseSchema *database_schema = NULL;
    if (OB_FAIL(schema_guard->get_database_schema(table_schema->get_database_id(),
                                                 database_schema))) {
    } else if (OB_ISNULL(database_schema)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ob_write_string(alloc, database_schema->get_database_name_str(),
                                       row.database_name_))) {
    } else if (OB_FAIL(ob_write_string(alloc, table_schema->get_table_name_str(),
                                       row.table_name_))) {
    } else if (OB_FAIL(build_rowkey_predicate(op, *table_schema, ctdef, current, alloc,
                                              row.rowkey_predicate_))) {
    } else if (ObDmlEventType::DE_UPDATING == event) {
      // The primary key may have moved: the task of the old row has to go.
      if (OB_FAIL(build_rowkey_predicate(op, *table_schema, ctdef, ctdef.old_row_, alloc,
                                         row.old_rowkey_predicate_))) {
      } else if (OB_FAIL(collect_changed_ids(op, ctdef, alloc, row.changed_column_ids_))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(pending.push_back(row))) {
    }
  }
  if (OB_FAIL(ret)) {
    // A failure here must never fail the statement the user issued.
    ai_trace_impl("[ai] record failed table_id=%lu ret=%d\n",
                  static_cast<unsigned long>(table_id), ret);
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObAiColumnRuntime::flush_pending(ObTableModifyOp &op,
                                     const ObAiPendingRowArray &pending,
                                     common::ObIAllocator &alloc,
                                     bool &has_work)
{
  int ret = OB_SUCCESS;
  has_work = false;
  ai_trace_impl("[ai] flush enter v3 pending=%ld\n", pending.count());
  if (pending.empty()) {
  } else {
    ObSEArray<uint64_t, 8> table_ids;
    for (int64_t i = 0; OB_SUCC(ret) && i < pending.count(); ++i) {
      bool seen = false;
      for (int64_t k = 0; !seen && k < table_ids.count(); ++k) {
        seen = (table_ids.at(k) == pending.at(i).table_id_);
      }
      if (!seen && OB_FAIL(table_ids.push_back(pending.at(i).table_id_))) {
      }
    }
    ObSEArray<AiColumnMeta, 8> metas;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(load_ai_columns(op, table_ids, alloc, metas))) {
      ai_trace_impl("[ai] flush load stage ret=%d tables=%ld\n", ret, table_ids.count());
    } else if (metas.empty()) {
      // None of the touched tables has an AI column.
    } else if (FALSE_IT(has_work = true)) {
    } else if (OB_FAIL(write_task_rows(op, pending, metas))) {
      ai_trace_impl("[ai] flush write stage ret=%d pending=%ld metas=%ld\n",
                    ret, pending.count(), metas.count());
    }
    if (OB_FAIL(ret)) {
      // The AI column must never break the statement the user issued.
      ai_trace_impl("[ai] flush failed ret=%d\n", ret);
    }
  }
  ret = OB_SUCCESS;
  return ret;
}


} // namespace sql
} // namespace oceanbase
