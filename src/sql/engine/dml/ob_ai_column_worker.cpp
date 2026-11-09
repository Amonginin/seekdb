#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/dml/ob_ai_column_worker.h"

#include <stdio.h>

#include "lib/allocator/page_arena.h"
#include "common/object/ob_object.h"
#include "lib/ob_define.h"
#include "lib/time/ob_time_utility.h"
#include "share/ob_errno.h"
#include "share/ob_server_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "oblib/common/mysqlclient/ob_mysql_proxy.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "query/api/query/ai/ob_ai_endpoint_resolver.h"

namespace oceanbase
{
namespace sql
{
using common::ObString;
using common::ObMySQLProxy;

namespace
{

const char *AI_COLUMN_TABLE = "oceanbase.__all_ai_column";
const char *AI_TASK_TABLE = "oceanbase.__all_ai_column_task";

// Task states shared with the DML side.
enum AiTaskState
{
  AI_TASK_PENDING = 0,
  AI_TASK_RUNNING = 1,
  AI_TASK_FAILED = 2,
  AI_TASK_RETRYING = 3,
};

// Doubles the characters that would end a SQL string literal.
int escape_literal(common::ObIAllocator &alloc, const ObString &in, ObString &out)
{
  int ret = OB_SUCCESS;
  if (in.empty()) {
    out.reset();
  } else {
    const int64_t capacity = in.length() * 2 + 1;
    char *buf = static_cast<char *>(alloc.alloc(capacity));
    int64_t pos = 0;
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < in.length(); ++i) {
        const char c = in.ptr()[i];
        if ('\'' == c || '\\' == c) {
          buf[pos++] = '\\';
        }
        buf[pos++] = c;
      }
      buf[pos] = '\0';
      out.assign_ptr(buf, static_cast<int32_t>(pos));
    }
  }
  return ret;
}

int run_sql_write(const char *sql)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql, affected_rows))) {
    LOG_WARN("failed to run the ai column statement", K(ret), KCSTRING(sql));
  }
  return ret;
}

// Reads the argument expression and the model of one AI column.
int load_column_meta(common::ObIAllocator &alloc,
                     const uint64_t table_id,
                     const uint64_t column_id,
                     ObString &arg_expr_text,
                     ObString &ai_func_name,
                     ObString &model_name,
                     int64_t &max_retries)
{
  int ret = OB_SUCCESS;
  char sql[512];
  snprintf(sql, sizeof(sql),
           "select arg_expr_text, ai_func_name, model_name, max_retries "
           "from %s where table_id = %lu and column_id = %lu",
           AI_COLUMN_TABLE,
           static_cast<unsigned long>(table_id),
           static_cast<unsigned long>(column_id));
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    sqlclient::ObMySQLResult *result = NULL;
    if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql))) {
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result->next())) {
      if (OB_ITER_END == ret) {
        // The column definition is gone, nothing to compute.
        ret = OB_SUCCESS;
      }
    } else {
      ObString arg_text;
      ObString func_name;
      ObString model;
      // arg_expr_text is nullable: read it as an object so a null value does
      // not fail the task.
      ObObj arg_obj;
      if (OB_FAIL(result->get_obj(static_cast<int64_t>(0), arg_obj))) {
      } else if (arg_obj.is_string() && OB_FAIL(arg_obj.get_string(arg_text))) {
      } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(1), func_name))) {
      } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(2), model))) {
      } else if (OB_FAIL(result->get_int(static_cast<int64_t>(3), max_retries))) {
      } else if (OB_FAIL(ob_write_string(alloc, arg_text, arg_expr_text))) {
      } else if (OB_FAIL(ob_write_string(alloc, func_name, ai_func_name))) {
      } else if (OB_FAIL(ob_write_string(alloc, model, model_name))) {
      }
    }
  }
  return ret;
}

// Evaluates the argument expression of the AI column for the target row.
int load_prompt(common::ObIAllocator &alloc,
                const ObString &database_name,
                const ObString &table_name,
                const ObString &arg_expr_text,
                const ObString &rowkey_predicate,
                ObString &prompt)
{
  int ret = OB_SUCCESS;
  char *sql = NULL;
  const int64_t capacity = arg_expr_text.length() + database_name.length()
      + table_name.length() + rowkey_predicate.length() + 64;
  if (OB_ISNULL(sql = static_cast<char *>(alloc.alloc(capacity)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    snprintf(sql, capacity, "select %.*s from `%.*s`.`%.*s` where %.*s",
             arg_expr_text.length(), arg_expr_text.ptr(),
             database_name.length(), database_name.ptr(),
             table_name.length(), table_name.ptr(),
             rowkey_predicate.length(), rowkey_predicate.ptr());
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("the row of the ai column task is gone", K(ret));
        }
      } else {
        ObString value;
        if (OB_FAIL(result->get_varchar(static_cast<int64_t>(0), value))) {
          if (OB_ERR_NULL_VALUE == ret) {
            ret = OB_SUCCESS;
          }
        } else if (OB_FAIL(ob_write_string(alloc, value, prompt))) {
        }
      }
    }
  }
  return ret;
}

// Calls the model of one AI column and returns its answer.
int call_model(common::ObIAllocator &alloc,
               const ObString &ai_func_name,
               const ObString &model_name,
               const ObString &prompt,
               ObString &result_text)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = NULL;
  share::ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *resolver = NULL;
  if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(alloc, model_name, info))) {
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(resolver = ::oceanbase::share::server_service<
                           ::oceanbase::query::ObIAiEndpointResolver>())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(resolver->resolve_by_model_name(model_name, alloc, endpoint))) {
  } else {
    ObAIFuncModel model(alloc, *info, endpoint);
    ObString prompt_copy = prompt;
    if (0 == ai_func_name.compare(ObString::make_string("ai_complete"))) {
      if (OB_FAIL(model.call_completion(prompt_copy, NULL, result_text))) {
      }
    } else if (0 == ai_func_name.compare(ObString::make_string("ai_embed"))) {
      if (OB_FAIL(model.call_dense_embedding(prompt_copy, NULL, result_text))) {
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported ai column function", K(ret), K(ai_func_name));
    }
  }
  return ret;
}

} // namespace

ObAiColumnWorker::ObAiColumnWorker()
  : common::ObTimerTask()
{
}

int ObAiColumnWorker::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObAiColumnWorker::runTimerTask()
{
  int ret = run_once();
  if (OB_FAIL(ret)) {
    LOG_WARN("ai column worker round failed", K(ret));
  }
}

int ObAiColumnWorker::run_once()
{
  int ret = OB_SUCCESS;
  char sql[1024];
  alloc_.reset();
  snprintf(sql, sizeof(sql),
           "select t.table_id, t.request_id, t.column_id, t.column_name, "
           "t.rowkey_predicate, t.retry_count, d.database_name, tb.table_name "
           "from %s t "
           "join oceanbase.__all_table tb on tb.table_id = t.table_id "
           "join oceanbase.__all_database d on d.database_id = tb.database_id "
           "where t.state in (%d, %d) and (t.eligible_ts is null or t.eligible_ts <= now(6)) "
           "order by t.table_id, t.request_id "
           "limit %ld",
           AI_TASK_TABLE, AI_TASK_PENDING, AI_TASK_RETRYING, MAX_TASKS_PER_ROUND);
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    sqlclient::ObMySQLResult *result = NULL;
    if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql))) {
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        uint64_t table_id = OB_INVALID_ID;
        uint64_t column_id = OB_INVALID_ID;
        ObString request_id;
        ObString column_name;
        ObString predicate;
        ObString db_name;
        ObString table_name;
        int64_t retry_count = 0;
        if (OB_FAIL(result->get_uint(static_cast<int64_t>(0), table_id))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(1), request_id))) {
        } else if (OB_FAIL(result->get_uint(static_cast<int64_t>(2), column_id))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(3), column_name))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(4), predicate))) {
        } else if (OB_FAIL(result->get_int(static_cast<int64_t>(5), retry_count))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(6), db_name))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(7), table_name))) {
        } else {
          int task_ret = process_group(table_id, request_id, column_id, column_name,
                                       predicate, db_name, table_name, retry_count);
          if (OB_FAIL(task_ret)) {
            // A single failing column must not stop the other tasks.
            LOG_WARN("failed to process an ai column task", K(task_ret), K(table_id),
                     K(column_id));
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::process_group(const uint64_t table_id,
                                    const ObString &request_id,
                                    const uint64_t column_id,
                                    const ObString &column_name,
                                    const ObString &rowkey_predicate,
                                    const ObString &database_name,
                                    const ObString &table_name,
                                    const int64_t retry_count)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR, OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObString arg_expr_text;
  ObString ai_func_name;
  ObString model_name;
  int64_t max_retries = 0;
  if (OB_FAIL(load_column_meta(alloc, table_id, column_id, arg_expr_text,
                               ai_func_name, model_name, max_retries))) {
  } else if (arg_expr_text.empty() || model_name.empty()) {
    // Nothing to evaluate, drop the task.
    ret = drop_task(table_id, request_id, column_id);
  } else {
    ObString prompt;
    ObString answer;
    if (OB_FAIL(load_prompt(alloc, database_name, table_name, arg_expr_text,
                            rowkey_predicate, prompt))) {
    } else if (OB_FAIL(call_model(alloc, ai_func_name, model_name, prompt, answer))) {
    }
    if (OB_SUCC(ret)) {
      ret = publish(table_id, request_id, column_id, column_name,
                    rowkey_predicate, database_name, table_name, answer);
    } else {
      // Keep the task and let the next round retry it.
      const int fail_ret = ret;
      ret = OB_SUCCESS;
      if (OB_FAIL(record_failure(table_id, request_id, column_id, retry_count,
                                 max_retries, fail_ret))) {
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::publish(const uint64_t table_id,
                              const ObString &request_id,
                              const uint64_t column_id,
                              const ObString &column_name,
                              const ObString &rowkey_predicate,
                              const ObString &database_name,
                              const ObString &table_name,
                              const ObString &answer)
{
  int ret = OB_SUCCESS;
  char *sql = NULL;
  const int64_t capacity = answer.length() * 2 + column_name.length() * 4
      + request_id.length() * 2 + database_name.length() + table_name.length()
      + rowkey_predicate.length() + 512;
  if (OB_ISNULL(sql = static_cast<char *>(alloc_.alloc(capacity)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    snprintf(sql, capacity,
             "update `%.*s`.`%.*s` set `%.*s` = '%.*s', "
             "`__ai_state_%.*s` = 'DONE', `__ai_req_%.*s` = '%.*s' "
             "where %.*s and `__ai_req_%.*s` = '%.*s'",
             database_name.length(), database_name.ptr(),
             table_name.length(), table_name.ptr(),
             column_name.length(), column_name.ptr(),
             answer.length(), answer.ptr(),
             column_name.length(), column_name.ptr(),
             column_name.length(), column_name.ptr(),
             request_id.length(), request_id.ptr(),
             rowkey_predicate.length(), rowkey_predicate.ptr(),
             column_name.length(), column_name.ptr(),
             request_id.length(), request_id.ptr());
    if (OB_FAIL(run_sql_write(sql))) {
    } else if (OB_FAIL(drop_task(table_id, request_id, column_id))) {
    }
  }
  return ret;
}

int ObAiColumnWorker::record_failure(const uint64_t table_id,
                                     const ObString &request_id,
                                     const uint64_t column_id,
                                     const int64_t retry_count,
                                     const int64_t max_retries,
                                     const int error_code)
{
  int ret = OB_SUCCESS;
  char sql[1024];
  const bool can_retry = retry_count < max_retries;
  snprintf(sql, sizeof(sql),
           "update %s set state = %d, retry_count = %ld, last_error = '%d', "
           "eligible_ts = now(6) + interval 1 second where table_id = %lu "
           "and request_id = '%.*s' and column_id = %lu",
           AI_TASK_TABLE,
           can_retry ? AI_TASK_RETRYING : AI_TASK_FAILED,
           static_cast<long>(retry_count + (can_retry ? 1 : 0)),
           error_code,
           static_cast<unsigned long>(table_id),
           request_id.length(), request_id.ptr(),
           static_cast<unsigned long>(column_id));
  if (OB_FAIL(run_sql_write(sql))) {
  }
  return ret;
}

int ObAiColumnWorker::drop_task(const uint64_t table_id,
                                const ObString &request_id,
                                const uint64_t column_id)
{
  char sql[512];
  snprintf(sql, sizeof(sql),
           "delete from %s where table_id = %lu and request_id = '%.*s' "
           "and column_id = %lu",
           AI_TASK_TABLE,
           static_cast<unsigned long>(table_id),
           request_id.length(), request_id.ptr(),
           static_cast<unsigned long>(column_id));
  return run_sql_write(sql);
}

} // namespace sql
} // namespace oceanbase
