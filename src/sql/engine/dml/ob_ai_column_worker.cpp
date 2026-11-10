#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/dml/ob_ai_column_worker.h"

#include <stdio.h>

#include "common/object/ob_object.h"
#include "common/object/ob_obj_type.h"
#include "lib/ob_define.h"
#include "sql/engine/dml/ob_ai_column_runtime.h"
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
using common::ObAIFuncExprInfo;
using common::ObAIFuncModel;
using common::ObAIFuncUtils;

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

// One AI column of one row, from the definition to the answer.
struct AiColumnPlan
{
  AiColumnPlan()
    : column_id_(common::OB_INVALID_ID),
      max_retries_(0),
      result_type_(0)
  {}
  uint64_t column_id_;
  ObString column_name_;
  ObString arg_expr_text_;
  ObString ai_func_name_;
  ObString model_name_;
  int64_t max_retries_;
  int64_t result_type_;
  ObString prompt_;
  ObString answer_;
  ObString system_prompt_;
};

// One task row of oceanbase.__all_ai_column_task.
struct AiTaskRow
{
  AiTaskRow()
    : table_id_(common::OB_INVALID_ID),
      column_id_(common::OB_INVALID_ID),
      retry_count_(0),
      state_(AI_TASK_PENDING),
      eligible_ts_(0)
  {}
  uint64_t table_id_;
  uint64_t column_id_;
  ObString request_id_;
  ObString column_name_;
  ObString rowkey_predicate_;
  ObString database_name_;
  ObString table_name_;
  int64_t retry_count_;
  int64_t state_;
  // The time the task becomes due, in microseconds since the epoch. Zero
  // means that it is due right away.
  int64_t eligible_ts_;
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

// Reads the argument expression, the model and the result type of one AI
// column.
int load_column_meta(common::ObIAllocator &alloc,
                     const uint64_t table_id,
                     const uint64_t column_id,
                     ObString &arg_expr_text,
                     ObString &ai_func_name,
                     ObString &model_name,
                     int64_t &max_retries,
                     int64_t &result_type)
{
  int ret = OB_SUCCESS;
  char sql[512];
  snprintf(sql, sizeof(sql),
           "select arg_expr_text, ai_func_name, model_name, max_retries, result_type "
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
      ObObj arg_obj;
      ObObj func_obj;
      ObObj model_obj;
      ObObj retry_obj;
      ObObj type_obj;
      ObString arg_text;
      ObString func_name;
      ObString model;
      // get_obj() reports a null value as success, while the typed getters
      // reject it. Every column of the definition is nullable.
      if (OB_FAIL(result->get_obj(static_cast<int64_t>(0), arg_obj))) {
      } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(1), func_obj))) {
      } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(2), model_obj))) {
      } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(3), retry_obj))) {
      } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(4), type_obj))) {
      } else if (arg_obj.is_string_type() && OB_FAIL(arg_obj.get_string(arg_text))) {
      } else if (func_obj.is_string_type() && OB_FAIL(func_obj.get_string(func_name))) {
      } else if (model_obj.is_string_type() && OB_FAIL(model_obj.get_string(model))) {
      } else if (OB_FAIL(ob_write_string(alloc, arg_text, arg_expr_text))) {
      } else if (OB_FAIL(ob_write_string(alloc, func_name, ai_func_name))) {
      } else if (OB_FAIL(ob_write_string(alloc, model, model_name))) {
      } else if (retry_obj.is_integer_type() && OB_FAIL(retry_obj.get_int(max_retries))) {
      } else if (type_obj.is_integer_type() && OB_FAIL(type_obj.get_int(result_type))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
    ob_ai_trace("[aiw] meta failed ret=%d tid=%lu cid=%lu\n", ret,
                static_cast<unsigned long>(table_id),
                static_cast<unsigned long>(column_id));
  }
  return ret;
}

// Builds the system prompt that tells the model which shape the answer must
// have. The column type decides it: a json column needs a json answer and an
// integer column needs a number.
int build_system_prompt(common::ObIAllocator &alloc,
                        const int64_t result_type,
                        ObString &system_prompt)
{
  int ret = OB_SUCCESS;
  const char *text = "";
  switch (result_type) {
    case common::ObJsonType:
      text = "Answer with a valid JSON value and nothing else.";
      break;
    case common::ObTinyIntType:
    case common::ObSmallIntType:
    case common::ObMediumIntType:
    case common::ObInt32Type:
    case common::ObIntType:
    case common::ObUTinyIntType:
    case common::ObUSmallIntType:
    case common::ObUMediumIntType:
    case common::ObUInt32Type:
    case common::ObUInt64Type:
      text = "Answer with a base-10 integer and nothing else.";
      break;
    default:
      break;
  }
  if (OB_FAIL(ob_write_string(alloc, ObString::make_string(text), system_prompt))) {
  }
  return ret;
}

// Evaluates the argument expressions of the request for the target row. One
// statement reads the value of every column of the request.
int load_prompts(common::ObIAllocator &alloc,
                 const ObString &database_name,
                 const ObString &table_name,
                 const ObString &rowkey_predicate,
                 common::ObIArray<AiColumnPlan> &plans)
{
  int ret = OB_SUCCESS;
  char *sql = NULL;
  int64_t capacity = database_name.length() + table_name.length()
      + rowkey_predicate.length() + 128;
  for (int64_t i = 0; i < plans.count(); ++i) {
    capacity += plans.at(i).arg_expr_text_.length() + 8;
  }
  if (OB_ISNULL(sql = static_cast<char *>(alloc.alloc(capacity)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t pos = snprintf(sql, capacity, "select ");
    for (int64_t i = 0; OB_SUCC(ret) && i < plans.count(); ++i) {
      const int64_t written = snprintf(sql + pos, capacity - pos, "%s%.*s",
                                       i == 0 ? "" : ", ",
                                       plans.at(i).arg_expr_text_.length(),
                                       plans.at(i).arg_expr_text_.ptr());
      if (OB_UNLIKELY(written <= 0 || pos + written >= capacity)) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        pos += written;
      }
    }
    if (OB_SUCC(ret)) {
      const int64_t written = snprintf(sql + pos, capacity - pos,
                                       " from `%.*s`.`%.*s` where %.*s",
                                       database_name.length(), database_name.ptr(),
                                       table_name.length(), table_name.ptr(),
                                       rowkey_predicate.length(), rowkey_predicate.ptr());
      if (OB_UNLIKELY(written <= 0 || pos + written >= capacity)) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        pos += written;
      }
    }
    if (OB_SUCC(ret)) {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        sqlclient::ObMySQLResult *result = NULL;
        if (OB_FAIL(GCTX.sql_proxy_->read(res, sql))) {
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(result->next())) {
          if (OB_ITER_END == ret) {
            // The row is gone, so there is nothing to compute.
            ret = OB_ITER_END;
          }
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < plans.count(); ++i) {
            ObObj value_obj;
            ObString value;
            if (OB_FAIL(result->get_obj(i, value_obj))) {
            } else if (value_obj.is_string_type()
                       && OB_FAIL(value_obj.get_string(value))) {
            } else if (OB_FAIL(ob_write_string(alloc, value, plans.at(i).prompt_))) {
            }
          }
        }
      }
    }
  }
  if (OB_FAIL(ret) && OB_ITER_END != ret) {
    ob_ai_trace("[aiw] prompts failed ret=%d sql=%s\n", ret, OB_ISNULL(sql) ? "" : sql);
  }
  return ret;
}

// Calls the model of one AI column and returns its answer.
int call_model(common::ObIAllocator &alloc,
               const ObString &ai_func_name,
               const ObString &model_name,
               const ObString &system_prompt,
               const ObString &prompt,
               ObString &result_text)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = NULL;
  share::ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *resolver = NULL;
  if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(alloc, model_name, info))) {
    ob_ai_trace("[aiw] model info failed ret=%d model=%.*s\n", ret, model_name.length(),
                model_name.ptr());
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
    ob_ai_trace("[aiw] model info is null model=%.*s\n", model_name.length(),
                model_name.ptr());
  } else if (OB_ISNULL(resolver = ::oceanbase::share::server_service<
                           ::oceanbase::query::ObIAiEndpointResolver>())) {
    ret = OB_ERR_UNEXPECTED;
    ob_ai_trace("[aiw] no endpoint resolver\n");
  } else if (OB_FAIL(resolver->resolve_by_model_name(model_name, alloc, endpoint))) {
    ob_ai_trace("[aiw] resolve endpoint failed ret=%d model=%.*s\n", ret,
                model_name.length(), model_name.ptr());
  } else {
    ObAIFuncModel model(alloc, *info, endpoint);
    ObString system_copy = system_prompt;
    ObString prompt_copy = prompt;
    if (0 == ai_func_name.compare(ObString::make_string("ai_complete"))) {
      if (OB_FAIL(model.call_completion(system_copy, prompt_copy, NULL, result_text))) {
        ob_ai_trace("[aiw] completion failed ret=%d\n", ret);
      }
    } else if (0 == ai_func_name.compare(ObString::make_string("ai_embed"))) {
      if (OB_FAIL(model.call_dense_embedding(prompt_copy, NULL, result_text))) {
        ob_ai_trace("[aiw] embedding failed ret=%d\n", ret);
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported ai column function", K(ret), K(ai_func_name));
    }
  }
  return ret;
}

// Publishes the answers of one request in a single statement, so a reader
// never sees one column of a row updated and the other one not.
int publish_group(common::ObIAllocator &alloc,
                  const ObString &database_name,
                  const ObString &table_name,
                  const ObString &rowkey_predicate,
                  common::ObIArray<AiColumnPlan> &plans)
{
  int ret = OB_SUCCESS;
  char *sql = NULL;
  int64_t capacity = database_name.length() * 2 + table_name.length() * 2
      + rowkey_predicate.length() + 128;
  for (int64_t i = 0; i < plans.count(); ++i) {
    capacity += plans.at(i).column_name_.length() * 2 + plans.at(i).answer_.length() * 2 + 16;
  }
  if (OB_ISNULL(sql = static_cast<char *>(alloc.alloc(capacity)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t pos = snprintf(sql, capacity, "update `%.*s`.`%.*s` set ",
                            database_name.length(), database_name.ptr(),
                            table_name.length(), table_name.ptr());
    for (int64_t i = 0; OB_SUCC(ret) && i < plans.count(); ++i) {
      ObString escaped;
      if (OB_FAIL(escape_literal(alloc, plans.at(i).answer_, escaped))) {
      } else {
        const int64_t written = snprintf(sql + pos, capacity - pos, "%s`%.*s` = '%.*s'",
                                         i == 0 ? "" : ", ",
                                         plans.at(i).column_name_.length(),
                                         plans.at(i).column_name_.ptr(),
                                         escaped.length(),
                                         OB_ISNULL(escaped.ptr()) ? "" : escaped.ptr());
        if (OB_UNLIKELY(written <= 0 || pos + written >= capacity)) {
          ret = OB_SIZE_OVERFLOW;
        } else {
          pos += written;
        }
      }
    }
    if (OB_SUCC(ret)) {
      const int64_t written = snprintf(sql + pos, capacity - pos, " where %.*s",
                                       rowkey_predicate.length(), rowkey_predicate.ptr());
      if (OB_UNLIKELY(written <= 0 || pos + written >= capacity)) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        pos += written;
      }
    }
    if (OB_SUCC(ret)) {
      ob_ai_trace("[aiw] publish sql=%s\n", sql);
      ret = run_sql_write(sql);
      if (OB_FAIL(ret)) {
        ob_ai_trace("[aiw] publish failed ret=%d\n", ret);
      }
    }
  }
  return ret;
}

// True when a newer request has already replaced the task of this row. The
// answer of a stale request must not overwrite the answer of the new one.
bool is_stale_request(const uint64_t table_id,
                      const uint64_t column_id,
                      const ObString &rowkey_predicate,
                      const ObString &request_id)
{
  int ret = OB_SUCCESS;
  bool stale = false;
  char sql[1024];
  snprintf(sql, sizeof(sql),
           "select count(*) from %s where table_id = %lu and column_id = %lu "
           "and rowkey_predicate = '%.*s' and request_id <> '%.*s'",
           AI_TASK_TABLE,
           static_cast<unsigned long>(table_id),
           static_cast<unsigned long>(column_id),
           rowkey_predicate.length(), rowkey_predicate.ptr(),
           request_id.length(), request_id.ptr());
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    sqlclient::ObMySQLResult *result = NULL;
    ObObj count_obj;
    int64_t count = 0;
    if (OB_ISNULL(GCTX.sql_proxy_)) {
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql))) {
    } else if (OB_ISNULL(result = res.get_result())) {
    } else if (OB_FAIL(result->next())) {
      // No row means there is no newer request.
      stale = false;
    } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(0), count_obj))) {
    } else if (count_obj.is_integer_type() && OB_FAIL(count_obj.get_int(count))) {
    } else {
      stale = (count > 0);
    }
  }
  return stale;
}

// Removes the task rows of one request.
int drop_request(const uint64_t table_id, const ObString &request_id)
{
  int ret = OB_SUCCESS;
  char sql[512];
  snprintf(sql, sizeof(sql),
           "delete from %s where table_id = %lu and request_id = '%.*s'",
           AI_TASK_TABLE,
           static_cast<unsigned long>(table_id),
           request_id.length(), request_id.ptr());
  if (OB_FAIL(run_sql_write(sql))) {
  }
  return ret;
}

// Removes the task row of one column.
int drop_task(const uint64_t table_id,
              const ObString &request_id,
              const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  char sql[512];
  snprintf(sql, sizeof(sql),
           "delete from %s where table_id = %lu and request_id = '%.*s' "
           "and column_id = %lu",
           AI_TASK_TABLE,
           static_cast<unsigned long>(table_id),
           request_id.length(), request_id.ptr(),
           static_cast<unsigned long>(column_id));
  if (OB_FAIL(run_sql_write(sql))) {
  }
  return ret;
}

// Records the failure of one column of the request. A column that ran out of
// retries loses its task row, so a failed request leaves nothing behind.
int record_failure(const uint64_t table_id,
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
           "eligible_ts = now(6) + interval 2 second where table_id = %lu "
           "and request_id = '%.*s' and column_id = %lu",
           AI_TASK_TABLE,
           can_retry ? AI_TASK_RETRYING : AI_TASK_FAILED,
           static_cast<long>(retry_count + (can_retry ? 1 : 0)),
           error_code,
           static_cast<unsigned long>(table_id),
           request_id.length(), request_id.ptr(),
           static_cast<unsigned long>(column_id));
  if (OB_FAIL(run_sql_write(sql))) {
  } else if (!can_retry && OB_FAIL(drop_task(table_id, request_id, column_id))) {
  }
  return ret;
}

// Computes and publishes one request, which covers all the AI columns of one
// row.
int process_request(const common::ObIArray<AiTaskRow> &rows)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR, OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObSEArray<AiColumnPlan, 16> plans;
  for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
    const AiTaskRow &row = rows.at(i);
    AiColumnPlan plan;
    plan.column_id_ = row.column_id_;
    plan.column_name_ = row.column_name_;
    if (OB_FAIL(load_column_meta(alloc, row.table_id_, row.column_id_,
                                 plan.arg_expr_text_, plan.ai_func_name_,
                                 plan.model_name_, plan.max_retries_,
                                 plan.result_type_))) {
    } else if (plan.arg_expr_text_.empty() || plan.model_name_.empty()) {
      // The column definition is gone, nothing to compute.
      if (OB_FAIL(drop_task(row.table_id_, row.request_id_, row.column_id_))) {
      }
    } else if (OB_FAIL(build_system_prompt(alloc, plan.result_type_,
                                           plan.system_prompt_))) {
    } else if (OB_FAIL(plans.push_back(plan))) {
    }
  }
  if (OB_SUCC(ret) && plans.count() > 0) {
    const AiTaskRow &first = rows.at(0);
    // A newer request may have taken over one of the columns already. Its
    // answer is the only one that may be published for that column.
    ObSEArray<AiColumnPlan, 16> live;
    for (int64_t i = 0; OB_SUCC(ret) && i < plans.count(); ++i) {
      const AiColumnPlan &plan = plans.at(i);
      if (is_stale_request(first.table_id_, plan.column_id_,
                           first.rowkey_predicate_, first.request_id_)) {
        if (OB_FAIL(drop_task(first.table_id_, first.request_id_,
                              plan.column_id_))) {
        }
      } else if (OB_FAIL(live.push_back(plan))) {
      }
    }
    if (OB_SUCC(ret) && live.count() > 0) {
      const int prompt_ret = load_prompts(alloc, first.database_name_,
                                          first.table_name_,
                                          first.rowkey_predicate_, live);
      if (OB_ITER_END == prompt_ret) {
        // The row is gone.
        ret = drop_request(first.table_id_, first.request_id_);
      } else if (OB_FAIL(prompt_ret)) {
        ret = prompt_ret;
      } else {
        // A column whose expression has no value has nothing to ask about.
        ObSEArray<AiColumnPlan, 16> asked;
        for (int64_t i = 0; OB_SUCC(ret) && i < live.count(); ++i) {
          if (live.at(i).prompt_.empty()) {
            if (OB_FAIL(drop_task(first.table_id_, first.request_id_,
                                  live.at(i).column_id_))) {
            }
          } else if (OB_FAIL(asked.push_back(live.at(i)))) {
          }
        }
        if (OB_SUCC(ret) && asked.count() > 0) {
          int64_t failed = 0;
          int failed_ret = OB_SUCCESS;
          int64_t max_retries = asked.at(0).max_retries_;
          for (int64_t i = 0; i < asked.count(); ++i) {
            max_retries = (asked.at(i).max_retries_ < max_retries)
                        ? asked.at(i).max_retries_ : max_retries;
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < asked.count(); ++i) {
            AiColumnPlan &plan = asked.at(i);
            ObString answer;
            const int model_ret = call_model(alloc, plan.ai_func_name_,
                                             plan.model_name_, plan.system_prompt_,
                                             plan.prompt_, answer);
            if (OB_FAIL(model_ret)) {
              failed++;
              failed_ret = model_ret;
            } else if (OB_FAIL(ob_write_string(alloc, answer, plan.answer_))) {
            }
          }
          if (OB_SUCC(ret) && failed > 0) {
            // The request failed, so none of its columns is published. Every
            // column of the request waits for the next round, because they
            // share one request.
            for (int64_t i = 0; OB_SUCC(ret) && i < asked.count(); ++i) {
              if (OB_FAIL(record_failure(first.table_id_, first.request_id_,
                                         asked.at(i).column_id_,
                                         first.retry_count_, max_retries,
                                         failed_ret))) {
              }
            }
          } else if (OB_SUCC(ret)) {
            // The row may have changed while the model was answering. The
            // answer of a request that has been replaced must not be
            // published.
            bool stale = false;
            for (int64_t i = 0; OB_SUCC(ret) && i < asked.count(); ++i) {
              if (is_stale_request(first.table_id_, asked.at(i).column_id_,
                                   first.rowkey_predicate_, first.request_id_)) {
                stale = true;
              }
            }
            if (stale) {
              ret = drop_request(first.table_id_, first.request_id_);
            } else {
              ret = publish_group(alloc, first.database_name_, first.table_name_,
                                   first.rowkey_predicate_, asked);
              if (OB_SUCC(ret)) {
                ret = drop_request(first.table_id_, first.request_id_);
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

// Two task rows belong to the same request when they share the table and the
// request id.
bool is_same_request(const AiTaskRow &left, const AiTaskRow &right)
{
  return left.table_id_ == right.table_id_
      && 0 == left.request_id_.compare(right.request_id_);
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
    ob_ai_trace("[aiw] round failed ret=%d\n", ret);
    LOG_WARN("ai column worker round failed", K(ret));
  }
}

int ObAiColumnWorker::run_once()
{
  int ret = OB_SUCCESS;
  char sql[1024];
  alloc_.reset();
  // The task table carries the name of the database and of the table, so the
  // worker does not have to join the system tables. A join of an inner table
  // with the system tables fails with "tablet does not exist" in the session
  // of the server proxy.
  //
  // The rows are not filtered here. A task row is removed as soon as it is
  // done, so every row of the table is a task that still has to be worked on.
  // The time a task becomes due is compared in C++ instead of in SQL: the
  // column is a timestamp and the session of the server proxy has a time zone
  // of its own, which makes the comparison in SQL unreliable.
  snprintf(sql, sizeof(sql),
           "select table_id, request_id, column_id, column_name, rowkey_predicate, "
           "retry_count, database_name, table_name, state, eligible_ts "
           "from %s "
           "order by table_id, request_id, column_id "
           "limit %ld",
           AI_TASK_TABLE, MAX_TASKS_PER_ROUND);
  ObSEArray<AiTaskRow, 64> rows;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    sqlclient::ObMySQLResult *result = NULL;
    if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql))) {
      ob_ai_trace("[aiw] query failed ret=%d\n", ret);
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        AiTaskRow row;
        ObString request_id;
        ObString column_name;
        ObString predicate;
        ObString db_name;
        ObString table_name;
        ObObj due_obj;
        if (OB_FAIL(result->get_uint(static_cast<int64_t>(0), row.table_id_))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(1), request_id))) {
        } else if (OB_FAIL(result->get_uint(static_cast<int64_t>(2), row.column_id_))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(3), column_name))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(4), predicate))) {
        } else if (OB_FAIL(result->get_int(static_cast<int64_t>(5), row.retry_count_))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(6), db_name))) {
        } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(7), table_name))) {
        } else if (OB_FAIL(result->get_int(static_cast<int64_t>(8), row.state_))) {
        } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(9), due_obj))) {
          // The due time is a timestamp. A result of a different type does
          // not tell when the task is due, so the task is tried right away.
          ret = OB_SUCCESS;
        } else {
          // The due time is read as an object because the column is a
          // timestamp, and the getter of the result only knows datetime.
          const common::ObObjType due_type = due_obj.get_type();
          int64_t value = 0;
          int value_ret = OB_SUCCESS;
          if (common::ob_is_datetime(due_type)) {
            value_ret = due_obj.get_datetime(value);
          } else if (common::ObTimestampType == due_type) {
            value_ret = due_obj.get_timestamp(value);
          }
          if (OB_SUCC(value_ret)) {
            row.eligible_ts_ = value;
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(ob_write_string(alloc_, request_id, row.request_id_))) {
          } else if (OB_FAIL(ob_write_string(alloc_, column_name, row.column_name_))) {
          } else if (OB_FAIL(ob_write_string(alloc_, predicate, row.rowkey_predicate_))) {
          } else if (OB_FAIL(ob_write_string(alloc_, db_name, row.database_name_))) {
          } else if (OB_FAIL(ob_write_string(alloc_, table_name, row.table_name_))) {
          } else if (OB_FAIL(rows.push_back(row))) {
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  ob_ai_trace("[aiw] round rows=%ld ret=%d\n", rows.count(), ret);
  const int64_t now = ObTimeUtility::current_time();
  for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
    const AiTaskRow &row = rows.at(i);
    if (AI_TASK_FAILED == row.state_) {
      // The task ran out of retries. It is cleaned up here, because the
      // worker may have died before it could do it.
      ob_ai_trace("[aiw] drop failed task rid=%.*s\n", row.request_id_.length(),
                  row.request_id_.ptr());
      if (OB_FAIL(drop_task(row.table_id_, row.request_id_, row.column_id_))) {
      }
      continue;
    }
    if (AI_TASK_PENDING != row.state_ && AI_TASK_RETRYING != row.state_) {
      continue;
    }
    if (row.eligible_ts_ > 0 && row.eligible_ts_ > now) {
      // The task has to wait before it is tried again.
      continue;
    }
    ob_ai_trace("[aiw] task rid=%.*s cid=%lu state=%ld\n", row.request_id_.length(),
                row.request_id_.ptr(), static_cast<unsigned long>(row.column_id_),
                row.state_);
    // The rows of one request are neighbours, because the query orders them by
    // the request id.
    bool handled = false;
    for (int64_t k = 0; !handled && k < i; ++k) {
      handled = is_same_request(rows.at(k), rows.at(i));
    }
    if (handled) {
      continue;
    }
    ObSEArray<AiTaskRow, 32> request_rows;
    for (int64_t k = i; OB_SUCC(ret) && k < rows.count(); ++k) {
      if (is_same_request(rows.at(i), rows.at(k))) {
        if (OB_FAIL(request_rows.push_back(rows.at(k)))) {
          break;
        }
      }
    }
    if (OB_SUCC(ret)) {
      const int request_ret = process_request(request_rows);
      if (OB_FAIL(request_ret)) {
        // One failing request must not stop the other requests.
        ob_ai_trace("[aiw] request failed ret=%d\n", request_ret);
        LOG_WARN("failed to process an ai column request", K(request_ret));
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
