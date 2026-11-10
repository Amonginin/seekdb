#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/dml/ob_ai_column_worker.h"

#include <stdio.h>

#include "common/object/ob_object.h"
#include "common/object/ob_obj_type.h"
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
      wait_us_(0)
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
  // Microseconds until the task is due. Zero or less means it is due now.
  int64_t wait_us_;
};

// Reads an integer column of any width. The typed getters of the result only
// accept one exact type, so an int column read as int64 fails with an object
// type error. A null value reads as zero.
int obj_to_int(const common::ObObj &obj, int64_t &value)
{
  int ret = OB_SUCCESS;
  value = 0;
  if (obj.is_null()) {
  } else if (!obj.is_integer_type()) {
    ret = OB_OBJ_TYPE_ERROR;
  } else {
    switch (obj.get_type()) {
      case common::ObTinyIntType: {
        int8_t v = 0;
        if (OB_FAIL(obj.get_tinyint(v))) {
        } else {
          value = v;
        }
        break;
      }
      case common::ObSmallIntType: {
        int16_t v = 0;
        if (OB_FAIL(obj.get_smallint(v))) {
        } else {
          value = v;
        }
        break;
      }
      case common::ObMediumIntType:
      case common::ObInt32Type: {
        int32_t v = 0;
        if (OB_FAIL(obj.get_int32(v))) {
        } else {
          value = v;
        }
        break;
      }
      case common::ObIntType: {
        int64_t v = 0;
        if (OB_FAIL(obj.get_int(v))) {
        } else {
          value = v;
        }
        break;
      }
      case common::ObUTinyIntType: {
        uint8_t v = 0;
        if (OB_FAIL(obj.get_utinyint(v))) {
        } else {
          value = v;
        }
        break;
      }
      case common::ObUSmallIntType: {
        uint16_t v = 0;
        if (OB_FAIL(obj.get_usmallint(v))) {
        } else {
          value = v;
        }
        break;
      }
      case common::ObUMediumIntType:
      case common::ObUInt32Type: {
        uint32_t v = 0;
        if (OB_FAIL(obj.get_uint32(v))) {
        } else {
          value = v;
        }
        break;
      }
      case common::ObUInt64Type: {
        uint64_t v = 0;
        if (OB_FAIL(obj.get_uint64(v))) {
        } else {
          value = v;
        }
        break;
      }
      default: {
        ret = OB_OBJ_TYPE_ERROR;
        break;
      }
    }
  }
  return ret;
}

// Reads a string column of any character type. A null value reads as an empty
// string.
int obj_to_string(const common::ObObj &obj, ObString &value)
{
  int ret = OB_SUCCESS;
  value.reset();
  if (obj.is_null()) {
  } else if (!obj.is_string_type()) {
    ret = OB_OBJ_TYPE_ERROR;
  } else if (OB_FAIL(obj.get_string(value))) {
  }
  return ret;
}

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
      } else if (OB_FAIL(obj_to_string(arg_obj, arg_text))) {
      } else if (OB_FAIL(obj_to_string(func_obj, func_name))) {
      } else if (OB_FAIL(obj_to_string(model_obj, model))) {
      } else if (OB_FAIL(ob_write_string(alloc, arg_text, arg_expr_text))) {
      } else if (OB_FAIL(ob_write_string(alloc, func_name, ai_func_name))) {
      } else if (OB_FAIL(ob_write_string(alloc, model, model_name))) {
      } else if (OB_FAIL(obj_to_int(retry_obj, max_retries))) {
      } else if (OB_FAIL(obj_to_int(type_obj, result_type))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("failed to read the definition of an ai column", K(ret),
             K(table_id), K(column_id));
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
            } else if (OB_FAIL(obj_to_string(value_obj, value))) {
            } else if (OB_FAIL(ob_write_string(alloc, value, plans.at(i).prompt_))) {
            }
          }
        }
      }
    }
  }
  if (OB_FAIL(ret) && OB_ITER_END != ret) {
    LOG_WARN("failed to evaluate the argument expression of an ai column", K(ret));
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
    LOG_WARN("failed to read the ai model of an ai column", K(ret),
             K(model_name));
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the ai model of an ai column is gone", K(ret), K(model_name));
  } else if (OB_ISNULL(resolver = ::oceanbase::share::server_service<
                           ::oceanbase::query::ObIAiEndpointResolver>())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("no endpoint resolver for an ai column", K(ret), K(model_name));
  } else if (OB_FAIL(resolver->resolve_by_model_name(model_name, alloc, endpoint))) {
    LOG_WARN("failed to resolve the endpoint of an ai model", K(ret),
             K(model_name));
  } else {
    ObAIFuncModel model(alloc, *info, endpoint);
    ObString system_copy = system_prompt;
    ObString prompt_copy = prompt;
    if (0 == ai_func_name.compare(ObString::make_string("ai_complete"))) {
      if (OB_FAIL(model.call_completion(system_copy, prompt_copy, NULL, result_text))) {
        LOG_WARN("the completion of an ai column failed", K(ret), K(model_name));
      }
    } else if (0 == ai_func_name.compare(ObString::make_string("ai_embed"))) {
      if (OB_FAIL(model.call_dense_embedding(prompt_copy, NULL, result_text))) {
        LOG_WARN("the embedding of an ai column failed", K(ret), K(model_name));
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
      ret = run_sql_write(sql);
      if (OB_FAIL(ret)) {
        LOG_WARN("failed to publish the result of an ai column", K(ret));
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
    } else if (OB_FAIL(obj_to_int(count_obj, count))) {
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
            // The result of the call is a value, not the status of the round:
            // OB_FAIL would overwrite ret and skip the bookkeeping below.
            if (OB_SUCCESS != model_ret) {
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
  // The due time is a timestamp column, which cannot be read through the
  // proxy. It is turned into a number of microseconds by the query itself, so
  // the session time zone of the proxy does not affect the comparison.
  snprintf(sql, sizeof(sql),
           "select table_id, request_id, column_id, column_name, rowkey_predicate, "
           "retry_count, database_name, table_name, state, "
           "ifnull(cast(timestampdiff(microsecond, now(6), eligible_ts) as signed), 0) "
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
        ObObj id_obj;
        ObObj request_obj;
        ObObj column_obj;
        ObObj name_obj;
        ObObj predicate_obj;
        ObObj retry_obj;
        ObObj db_obj;
        ObObj table_obj;
        ObObj state_obj;
        ObObj wait_obj;
        int64_t id_value = 0;
        int64_t column_value = 0;
        if (OB_FAIL(result->get_obj(static_cast<int64_t>(0), id_obj))) {
        } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(1), request_obj))) {
        } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(2), column_obj))) {
        } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(3), name_obj))) {
        } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(4), predicate_obj))) {
        } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(5), retry_obj))) {
        } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(6), db_obj))) {
        } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(7), table_obj))) {
        } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(8), state_obj))) {
        } else if (OB_FAIL(result->get_obj(static_cast<int64_t>(9), wait_obj))) {
        } else if (OB_FAIL(obj_to_int(id_obj, id_value))) {
        } else if (OB_FAIL(obj_to_int(column_obj, column_value))) {
        } else if (OB_FAIL(obj_to_int(retry_obj, row.retry_count_))) {
        } else if (OB_FAIL(obj_to_int(state_obj, row.state_))) {
        } else if (OB_FAIL(obj_to_int(wait_obj, row.wait_us_))) {
        } else if (OB_FAIL(obj_to_string(request_obj, request_id))) {
        } else if (OB_FAIL(obj_to_string(name_obj, column_name))) {
        } else if (OB_FAIL(obj_to_string(predicate_obj, predicate))) {
        } else if (OB_FAIL(obj_to_string(db_obj, db_name))) {
        } else if (OB_FAIL(obj_to_string(table_obj, table_name))) {
        } else {
          row.table_id_ = static_cast<uint64_t>(id_value);
          row.column_id_ = static_cast<uint64_t>(column_value);
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
  for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
    const AiTaskRow &row = rows.at(i);
    if (AI_TASK_FAILED == row.state_) {
      // The task ran out of retries but the row is still there, which happens
      // when the worker could not remove it at the time.
      if (OB_FAIL(drop_task(row.table_id_, row.request_id_, row.column_id_))) {
      }
      continue;
    }
    if (row.wait_us_ > 0) {
      // The request is waiting for its next attempt.
      continue;
    }
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
      // The next request must still be tried, so the result of this one is not
      // written into the status of the round.
      if (OB_SUCCESS != request_ret) {
        // One failing request must not stop the other requests.
        LOG_WARN("failed to process an ai column request", K(request_ret));
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
