/**
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/dml/ob_file_table_resolver.h"

#include <string.h>

#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_file_parquet_reader.h"
#include "sql/resolver/dml/ob_file_table_util.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"
#include "sql/session/ob_sql_session_info.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
namespace sql
{

namespace
{

bool equals_ignore_case(const common::ObString &text, const char *word)
{
  const int64_t word_len = static_cast<int64_t>(strlen(word));
  bool result = (text.length() == word_len);
  for (int64_t i = 0; result && i < text.length(); ++i) {
    const char lhs = text.ptr()[i];
    const char rhs = word[i];
    const char lhs_lower = (lhs >= 'A' && lhs <= 'Z') ? (lhs - 'A' + 'a') : lhs;
    const char rhs_lower = (rhs >= 'A' && rhs <= 'Z') ? (rhs - 'A' + 'a') : rhs;
    if (lhs_lower != rhs_lower) {
      result = false;
    }
  }
  return result;
}

// 取节点的文本：不同字面量 token 填的字段不同（标识符给 raw_text_，
// 字符串给 str_value_），所以这里三级兜底，别只认一个。
int node_text(const ParseNode *node, common::ObString &out)
{
  int ret = OB_SUCCESS;
  if (NULL == node) {
    ret = OB_INVALID_ARGUMENT;
  } else if (NULL != node->str_value_ && node->str_len_ > 0) {
    out.assign_ptr(node->str_value_, node->str_len_);
  } else if (NULL != node->raw_text_ && node->text_len_ > 0) {
    out.assign_ptr(node->raw_text_, node->text_len_);
  } else if (T_INT == node->type_ || T_BOOL == node->type_) {
    // 交给调用方按数值处理
    ret = OB_INVALID_ARGUMENT;
  } else {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

// 取选项的文本值
int get_option_text(const ParseNode *value, common::ObString &out)
{
  int ret = OB_SUCCESS;
  if (NULL == value) {
    ret = OB_INVALID_ARGUMENT;
  } else if (T_OP_NEG == value->type_ && NULL != value->children_[0]) {
    ret = node_text(value->children_[0], out);
  } else {
    ret = node_text(value, out);
  }
  return ret;
}

// 取选项的整数值
int get_option_int(const ParseNode *value, int64_t &out)
{
  int ret = OB_SUCCESS;
  bool negative = false;
  const ParseNode *real_value = value;
  if (NULL == value) {
    ret = OB_INVALID_ARGUMENT;
  } else if (T_OP_NEG == value->type_) {
    negative = true;
    real_value = value->children_[0];
  }
  if (OB_FAIL(ret)) {
  } else if (NULL == real_value) {
    ret = OB_INVALID_ARGUMENT;
  } else if (T_INT == real_value->type_ || T_BOOL == real_value->type_) {
    out = real_value->value_;
  } else {
    common::ObString text;
    if (OB_FAIL(node_text(real_value, text))) {
      LOG_WARN("option value is not a number", K(ret));
    } else {
      out = strtoll(text.ptr(), NULL, 10);
    }
  }
  if (OB_SUCC(ret) && negative) {
    out = -out;
  }
  return ret;
}

int get_option_bool(const ParseNode *value, bool &out)
{
  int ret = OB_SUCCESS;
  if (NULL == value) {
    ret = OB_INVALID_ARGUMENT;
  } else if (T_BOOL == value->type_) {
    out = (0 != value->value_);
  } else {
    common::ObString text;
    if (OB_FAIL(get_option_text(value, text))) {
    } else if (equals_ignore_case(text, "true") || equals_ignore_case(text, "1")) {
      out = true;
    } else if (equals_ignore_case(text, "false") || equals_ignore_case(text, "0")) {
      out = false;
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

struct FileTableOptions
{
  FileTableOptions()
      : has_header_(true), format_given_(false), format_(), sample_size_given_(false),
        sample_size_(-1) {}
  bool has_header_;
  bool format_given_;
  common::ObString format_;
  bool sample_size_given_;
  int64_t sample_size_;
};

int parse_file_table_options(const ParseNode *options, FileTableOptions &parsed)
{
  int ret = OB_SUCCESS;
  const ParseNode *current = options;
  while (OB_SUCC(ret) && NULL != current) {
    const ParseNode *option = NULL;
    if (T_LINK_NODE == current->type_) {
      option = current->children_[1];
      current = current->children_[0];
    } else {
      option = current;
      current = NULL;
    }
    if (NULL == option || T_FILE_TABLE_OPTION != option->type_
        || 2 != option->num_child_) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid file table option node");
    } else {
      const ParseNode *name_node = option->children_[0];
      const ParseNode *value_node = option->children_[1];
      common::ObString name;
      if (NULL == name_node) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(node_text(name_node, name))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("file table option has no name", K(ret));
      }
      if (OB_FAIL(ret)) {
      } else if (equals_ignore_case(name, "header")) {
        if (NULL == value_node) {
          parsed.has_header_ = true;
        } else if (OB_FAIL(get_option_bool(value_node, parsed.has_header_))) {
          LOG_WARN("invalid header option", K(ret));
        }
      } else if (equals_ignore_case(name, "sample_size")) {
        parsed.sample_size_given_ = true;
        if (OB_FAIL(get_option_int(value_node, parsed.sample_size_))) {
          LOG_WARN("invalid sample_size option", K(ret));
        }
      } else if (equals_ignore_case(name, "format")) {
        parsed.format_given_ = true;
        if (OB_FAIL(get_option_text(value_node, parsed.format_))) {
          LOG_WARN("invalid format option", K(ret));
        }
      } else {
        // 未识别的选项一律报不支持，避免静默忽略
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "unknown file table option");
      }
    }
  }
  return ret;
}

// 从 select 的语法树里找出第一个文件表函数节点
const ParseNode *find_file_table_node(const ParseNode *node)
{
  const ParseNode *found = NULL;
  if (NULL != node) {
    if (T_READ_FILE_TABLE_EXPRESSION == node->type_) {
      found = node;
    } else if (node->num_child_ > 0 && NULL != node->children_) {
      for (int64_t i = 0; NULL == found && i < node->num_child_; ++i) {
        found = find_file_table_node(node->children_[i]);
      }
    }
  }
  return found;
}

// parquet 的列类型由文件自带，这里只做「物理类型 -> duck 类型」的映射与取值转文本
int materialize_parquet(ObParquetFile &parquet, ObFileTableData &data)
{
  int ret = OB_SUCCESS;
  const int64_t column_count = parquet.column_count();
  const int64_t row_count = parquet.row_count();
  if (column_count <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("parquet file has no column");
  }
  for (int64_t col = 0; OB_SUCC(ret) && col < column_count; ++col) {
    const ObParquetColumn &column = parquet.column(col);
    common::ObString name;
    ObFileDuckType duck_type = OB_FILE_DUCK_VARCHAR;
    if (OB_FAIL(data.copy_string(column.name_.ptr(), column.name_.length(), name))) {
    } else if (OB_FAIL(data.add_column(name))) {
    } else {
      switch (column.physical_type_)
      {
        case 0: duck_type = OB_FILE_DUCK_BOOLEAN; break;          // BOOLEAN
        case 1:                                                   // INT32
        case 2: duck_type = OB_FILE_DUCK_BIGINT; break;           // INT64
        case 4:                                                   // FLOAT
        case 5: duck_type = OB_FILE_DUCK_DOUBLE; break;           // DOUBLE
        case 6: duck_type = OB_FILE_DUCK_VARCHAR; break;          // BYTE_ARRAY
        default:
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "this parquet physical type");
          break;
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(data.set_column_type(col, duck_type, column.nullable_))) {
    }
  }
  for (int64_t row = 0; OB_SUCC(ret) && row < row_count; ++row) {
    for (int64_t col = 0; OB_SUCC(ret) && col < column_count; ++col) {
      const ObParquetColumn &column = parquet.column(col);
      const ObFileDuckType type = data.column(col).type_;
      if (OB_ISNULL(column.values_) || row >= column.values_->count()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const ObParquetValue &value = column.values_->at(row);
        if (value.is_null_) {
          ret = data.add_cell(NULL, 0, true);
        } else if (OB_FILE_DUCK_BOOLEAN == type) {
          const char *text = (0 != value.num_) ? "true" : "false";
          ret = data.add_cell(text, static_cast<int64_t>(strlen(text)), false);
        } else if (OB_FILE_DUCK_BIGINT == type) {
          char num_buf[64];
          const int64_t num_len = snprintf(num_buf, sizeof(num_buf), "%ld", value.num_);
          if (num_len <= 0) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            ret = data.add_cell(num_buf, num_len, false);
          }
        } else if (OB_FILE_DUCK_DOUBLE == type) {
          char num_buf[64];
          double real_value = 0.0;
          MEMCPY(&real_value, &value.num_, sizeof(real_value));
          const int64_t num_len = snprintf(num_buf, sizeof(num_buf), "%.17g", real_value);
          if (num_len <= 0) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            ret = data.add_cell(num_buf, num_len, false);
          }
        } else {
          ret = data.add_cell(value.str_.ptr(), value.str_.length(), false);
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(data.end_row())) {
    }
  }
  return ret;
}

// 读文件 + 解析 + 推断，产出物化的表数据
int build_file_data(const ParseNode &file_node,
                    common::ObIAllocator &alloc,
                    ObFileTableData &data)
{
  int ret = OB_SUCCESS;
  FileTableOptions options;
  const ParseNode *path_node = NULL;
  common::ObString path;
  char *buf = NULL;
  int64_t len = 0;
  const int64_t format = file_node.value_;
  if (3 != file_node.num_child_ || NULL == (path_node = file_node.children_[0])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid file table expression");
  } else {
    path.assign_ptr(path_node->str_value_, path_node->str_len_);
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(parse_file_table_options(file_node.children_[1], options))) {
  } else if (options.sample_size_given_ && -1 != options.sample_size_) {
    // 题面只允许全量推断
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size other than -1");
  } else if (options.format_given_
             && !equals_ignore_case(options.format_, "csv")
             && !equals_ignore_case(options.format_, "newline_delimited")
             && !equals_ignore_case(options.format_, "auto")) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "this file format");
  }

  if (OB_SUCC(ret)) {
    if (5 == format) {
      // glob 不读文件内容，只列目录
      if (OB_FAIL(ob_file_glob(path, alloc, data))) {
        LOG_WARN("glob failed", K(path), K(ret));
      }
    } else if (4 == format) {
      // parquet：footer 里的 schema 定列，页解码后取值转成文本
      if (OB_FAIL(ob_file_read_all(path, alloc, buf, len))) {
        LOG_WARN("read parquet file failed", K(path), K(ret));
      } else {
        ObParquetFile parquet(alloc);
        if (OB_FAIL(parquet.parse(buf, len))) {
          LOG_WARN("parse parquet failed", K(path), K(ret));
        } else if (OB_FAIL(materialize_parquet(parquet, data))) {
          LOG_WARN("materialize parquet failed", K(path), K(ret));
        }
      }
    } else if (OB_FAIL(ob_file_read_all(path, alloc, buf, len))) {
      LOG_WARN("read file failed", K(path), K(ret));
    } else if (1 == format) {
      if (OB_FAIL(ob_file_parse_csv(buf, len, options.has_header_, alloc, data))) {
        LOG_WARN("parse csv failed", K(path), K(ret));
      }
    } else {
      if (OB_FAIL(ob_file_parse_jsonl(buf, len, alloc, data))) {
        LOG_WARN("parse jsonl failed", K(path), K(ret));
      }
    }
  }
  return ret;
}

} // namespace

int ObDMLResolver::resolve_file_table_item(const ParseNode &parse_tree, TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  TableItem *item = NULL;
  bool enable_file_sql = false;
  common::ObSqlString sql;
  ParseNode *select_root = NULL;
  ParseNode *alias_wrapper = NULL;

  if (NULL == session_info_ || NULL == allocator_ || NULL == stmt) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(session_info_->get_enable_file_sql(enable_file_sql))) {
    LOG_WARN("get enable_file_sql failed", K(ret));
  } else if (!enable_file_sql) {
    // 系统变量默认关闭，未开启时按不支持处理（客户端拿到 1235）
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql, please set enable_file_sql = on");
  } else {
    ObFileTableData data(*allocator_);
    if (OB_FAIL(build_file_data(parse_tree, *allocator_, data))) {
    } else if (OB_FAIL(ob_file_build_select_sql(data, sql))) {
    } else if (sql.empty()) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      // 把物化出来的行重新解析成一段普通 select，复用既有 resolver
      ObParser parser(*allocator_,
                      session_info_->get_sql_mode(),
                      session_info_->get_charsets4parser());
      ParseResult parse_result;
      if (OB_FAIL(parser.parse(sql.string(), parse_result))) {
        LOG_WARN("parse generated file table sql failed", K(sql), K(ret));
      } else if (NULL == parse_result.result_tree_
                 || OB_ISNULL(select_root = parse_result.result_tree_->children_[0])) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(alias_wrapper = new_node(allocator_, T_ALIAS, 2))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        alias_wrapper->children_[0] = select_root;
        alias_wrapper->children_[1] = parse_tree.children_[2];
        if (OB_FAIL(ObDMLResolver::resolve_table(*alias_wrapper, item))) {
          LOG_WARN("resolve generated file table failed", K(ret));
        } else {
          table_item = item;
        }
      }
    }
  }
  return ret;
}

int ob_file_build_describe_sql_from_select(const ParseNode &select_node,
                                           common::ObIAllocator &alloc,
                                           common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const ParseNode *file_node = find_file_table_node(&select_node);
  if (NULL == file_node) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "describe without a file table");
  } else {
    ObFileTableData data(alloc);
    if (OB_FAIL(build_file_data(*file_node, alloc, data))) {
      LOG_WARN("build file data for describe failed", K(ret));
    } else if (OB_FAIL(ob_file_build_describe_sql(data, sql))) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
