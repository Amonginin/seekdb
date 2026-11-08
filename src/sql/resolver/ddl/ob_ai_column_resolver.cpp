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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/resolver/ddl/ob_ai_column_resolver.h"
#include "share/ob_ai_diag.h"
#include "lib/utility/utility.h"
#include "sql/parser/parse_node.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace sql
{

const char *const ObAiColumnResolver::JSON_TYPE_DIRECTIVE =
    "Return exactly one valid JSON value";
const char *const ObAiColumnResolver::INT_TYPE_DIRECTIVE =
    "Return exactly one base-10 integer";

namespace
{

bool is_ai_complete_func(const common::ObString &func_name)
{
  return 0 == func_name.compare(ObString::make_string("ai_complete"));
}

bool is_ai_embed_func(const common::ObString &func_name)
{
  return 0 == func_name.compare(ObString::make_string("ai_embed"));
}

// Copies a piece of text into the buffer that is being built.
int append_text(common::ObIAllocator &allocator,
                common::ObString &text,
                const char *value,
                const int64_t length)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(value) || length <= 0) {
    // nothing to append
  } else {
    const int64_t old_len = text.length();
    // A separator is inserted between two pieces of text, so the extra byte
    // only exists when there is already something in the buffer.
    const int64_t sep_len = old_len > 0 ? 1 : 0;
    const int64_t new_len = old_len + sep_len + length;
    char *buf = static_cast<char *>(allocator.alloc(new_len + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (old_len > 0) {
        MEMCPY(buf, text.ptr(), old_len);
        buf[old_len] = ' ';
      }
      MEMCPY(buf + old_len + sep_len, value, length);
      buf[new_len] = '\0';
      text.assign_ptr(buf, new_len);
    }
  }
  return ret;
}

int append_text(common::ObIAllocator &allocator,
                common::ObString &text,
                const common::ObString &value)
{
  return append_text(allocator, text, value.ptr(), value.length());
}

// Rebuilds a readable text for a parse node. Only the shapes an AI column
// expression can take are supported; anything else falls back to a marker so
// the persisted expression stays well formed.
int build_node_text(common::ObIAllocator &allocator,
                     ParseNode *node,
                     common::ObString &text)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    // nothing to append
  } else if (node->type_ == T_IDENT) {
    if (OB_FAIL(append_text(allocator, text, node->str_value_, node->str_len_))) {
    }
  } else if (node->type_ == T_VARCHAR) {
    if (OB_FAIL(append_text(allocator, text, node->str_value_, node->str_len_))) {
    }
  } else if (node->type_ == T_INT || node->type_ == T_NUMBER) {
    if (OB_FAIL(append_text(allocator, text, node->str_value_, node->str_len_))) {
    }
  } else if (node->type_ == T_EXPR_LIST) {
    for (int32_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      if (OB_FAIL(build_node_text(allocator, node->children_[i], text))) {
      }
      if (OB_SUCC(ret) && i + 1 < node->num_child_) {
        if (OB_FAIL(append_text(allocator, text, ",", 1))) {
        }
      }
    }
  } else if (node->type_ == T_COLUMN_REF) {
    // A column reference carries (relation, table, column); the column name is
    // the last child.
    ParseNode *name_node = node->num_child_ > 2 ? node->children_[node->num_child_ - 1] : NULL;
    if (OB_NOT_NULL(name_node) && OB_NOT_NULL(name_node->str_value_)) {
      if (OB_FAIL(append_text(allocator, text, name_node->str_value_, name_node->str_len_))) {
      }
    }
  } else if (node->type_ == T_FUN_SYS) {
    if (OB_FAIL(build_node_text(allocator, node->children_[0], text))) {
    } else if (OB_FAIL(append_text(allocator, text, "(", 1))) {
    } else if (node->num_child_ > 1 && OB_NOT_NULL(node->children_[1])) {
      if (OB_FAIL(build_node_text(allocator, node->children_[1], text))) {
      }
    } else if (OB_FAIL(append_text(allocator, text, ")", 1))) {
    }
  } else if (node->num_child_ > 0) {
    // A generic node: render the first child, which is usually the operator
    // or the operand that carries the meaning.
    if (OB_FAIL(build_node_text(allocator, node->children_[0], text))) {
    }
  }
  return ret;
}

// Collects the names of the columns an AI expression references.
int collect_column_refs(common::ObIAllocator &allocator,
                        ParseNode *node,
                        common::ObSEArray<common::ObString, 8> &refs)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    // nothing to do
  } else if (node->type_ == T_COLUMN_REF) {
    // A column reference carries (relation, table, column); the column name is
    // the last child.
    ParseNode *name_node = node->num_child_ > 2 ? node->children_[node->num_child_ - 1] : NULL;
    if (OB_NOT_NULL(name_node) && OB_NOT_NULL(name_node->str_value_)) {
      common::ObString raw_name(name_node->str_len_, name_node->str_value_);
      common::ObString name;
      if (OB_FAIL(ob_write_string(allocator, raw_name, name))) {
      } else if (OB_FAIL(refs.push_back(name))) {
      }
    }
  } else {
    for (int32_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      if (OB_FAIL(collect_column_refs(allocator, node->children_[i], refs))) {
      }
    }
  }
  return ret;
}
} // namespace

int ObAiColumnResolver::parse_ai_expr(common::ObIAllocator &allocator,
                                        ParseNode *ai_expr,
                                        common::ObString &func_name,
                                        common::ObString &model_name,
                                        common::ObString &expr_text,
                                        int64_t &dim)
{
  int ret = OB_SUCCESS;
  dim = OB_INVALID_ID;
  ObAiDiag::trace("AI parse_expr: node=%p type=%d num_child=%d\n",
                  (void *)ai_expr, ai_expr ? (int)ai_expr->type_ : -1,
                  ai_expr ? ai_expr->num_child_ : -1);
  if (OB_ISNULL(ai_expr) || ai_expr->type_ != T_FUN_SYS || ai_expr->num_child_ < 1) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN requires an AI function call");
    } else {
      ParseNode *name_node = ai_expr->children_[0];
      ObAiDiag::trace("AI parse_expr: name_node=%p type=%d str=%p len=%ld\n",
                      (void *)name_node, name_node ? (int)name_node->type_ : -1,
                      name_node ? name_node->str_value_ : NULL,
                      name_node ? name_node->str_len_ : -1);
      if (OB_ISNULL(name_node) || OB_ISNULL(name_node->str_value_)) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN requires an AI function call");
      } else {
        common::ObString raw_name(name_node->str_len_, name_node->str_value_);
        common::ObString lower_name;
        if (OB_FAIL(ob_write_string(allocator, raw_name, lower_name))) {
        } else if (0 != lower_name.compare(ObString::make_string("AI_COMPLETE"))
                   && 0 != lower_name.compare(ObString::make_string("AI_EMBED"))) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN only supports AI_COMPLETE and AI_EMBED");
        } else {
          // Lower case the function name so the metadata is stable.
          for (int64_t i = 0; i < lower_name.length(); ++i) {
            char &c = const_cast<char &>(lower_name.ptr()[i]);
            if (c >= 'A' && c <= 'Z') {
              c = static_cast<char>(c - 'A' + 'a');
            }
          }
          func_name = lower_name;
          ParseNode *arg_list = ai_expr->num_child_ > 1 ? ai_expr->children_[1] : NULL;
          ObAiDiag::trace("AI parse_expr: func=%.*s arg_list=%p type=%d cnt=%d\n",
                          lower_name.length(), lower_name.ptr(),
                          (void *)arg_list, arg_list ? (int)arg_list->type_ : -1,
                          arg_list ? arg_list->num_child_ : -1);
          if (OB_ISNULL(arg_list) || arg_list->num_child_ < 1) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN requires at least a model name");
          } else {
            ParseNode *model_node = arg_list->children_[0];
            ObAiDiag::trace("AI parse_expr: model_node=%p type=%d str=%p\n",
                            (void *)model_node, model_node ? (int)model_node->type_ : -1,
                            model_node ? model_node->str_value_ : NULL);
            if (OB_ISNULL(model_node) || OB_ISNULL(model_node->str_value_)) {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN requires a model name");
            } else {
              common::ObString raw_model(model_node->str_len_, model_node->str_value_);
              if (OB_FAIL(ob_write_string(allocator, raw_model, model_name))) {
              } else {
                if (is_ai_embed_func(func_name) && arg_list->num_child_ >= 3) {
                  ParseNode *dim_node = arg_list->children_[2];
                  if (OB_NOT_NULL(dim_node) && OB_NOT_NULL(dim_node->str_value_)) {
                    dim = strtoll(dim_node->str_value_, NULL, 10);
                  }
                }
                if (OB_SUCC(ret)) {
                  ret = build_expr_text(allocator, ai_expr, expr_text);
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

// Reads one option node: the key is the first child, the value the second one.
int ObAiColumnResolver::parse_one_option(ParseNode *opt,
                                         common::ObString &sync_mode,
                                         int64_t &max_retries)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(opt) || opt->type_ != T_AI_COLUMN_OPTION || opt->num_child_ < 2) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected ai column option", K(ret));
  } else {
    ParseNode *key = opt->children_[0];
    ParseNode *value = opt->children_[1];
    if (OB_ISNULL(key) || OB_ISNULL(key->str_value_) || OB_ISNULL(value)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected ai column option content", K(ret));
    } else {
      common::ObString opt_name(key->str_len_, key->str_value_);
      if (0 == opt_name.compare(ObString::make_string("sync_mode"))) {
        if (OB_ISNULL(value->str_value_)) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN sync_mode must be a string");
        } else {
          sync_mode = common::ObString(value->str_len_, value->str_value_);
        }
      } else if (0 == opt_name.compare(ObString::make_string("max_retries"))) {
        if (OB_ISNULL(value->str_value_)) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN max_retries must be an integer");
        } else {
          max_retries = strtoll(value->str_value_, NULL, 10);
          if (max_retries < 0) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN max_retries must not be negative");
          }
        }
      }
    }
  }
  return ret;
}

// Walks the option list. The grammar builds a left recursive list, so the
// options may sit behind link nodes; they are visited recursively.
int ObAiColumnResolver::collect_ai_options(ParseNode *node,
                                            common::ObString &sync_mode,
                                            int64_t &max_retries)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    // nothing to do
  } else if (node->type_ == T_AI_COLUMN_OPTION) {
    ret = parse_one_option(node, sync_mode, max_retries);
  } else if (node->type_ == T_LINK_NODE || node->type_ == T_AI_COLUMN_OPTION_LIST) {
    for (int32_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      if (OB_FAIL(collect_ai_options(node->children_[i], sync_mode, max_retries))) {
      }
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected ai column option node", K(ret), K(node->type_));
  }
  return ret;
}

int ObAiColumnResolver::parse_ai_options(ParseNode *option_node,
                                          common::ObString &sync_mode,
                                          int64_t &max_retries)
{
  int ret = OB_SUCCESS;
  max_retries = 0;
  if (OB_ISNULL(option_node)) {
    // no options, the defaults stay in place
  } else {
    ObAiDiag::trace("AI parse_opts: option_node type=%d cnt=%d\n",
                    (int)option_node->type_, option_node->num_child_);
    ret = collect_ai_options(option_node, sync_mode, max_retries);
  }
  return ret;
}

int ObAiColumnResolver::build_expr_text(common::ObIAllocator &allocator,
                                         ParseNode *node,
                                         common::ObString &expr_text)
{
  common::ObString text;
  int ret = build_node_text(allocator, node, text);
  if (OB_SUCC(ret)) {
    expr_text = text;
  }
  return ret;
}

int ObAiColumnResolver::build_normalized_expr(common::ObIAllocator &allocator,
                                               const common::ObString &expr_text,
                                               const ObDataType &column_type,
                                               common::ObString &normalized_expr)
{
  int ret = OB_SUCCESS;
  const char *directive = NULL;
  if (ob_is_json(column_type.get_obj_type())) {
    directive = JSON_TYPE_DIRECTIVE;
  } else if (ob_is_integer_type(column_type.get_obj_type())) {
    directive = INT_TYPE_DIRECTIVE;
  }
  if (OB_ISNULL(directive)) {
    // Other result types do not need a type directive.
    normalized_expr = expr_text;
  } else {
    common::ObString tail;
    common::ObString raw_tail(strlen(directive), directive);
    if (OB_FAIL(ob_write_string(allocator, raw_tail, tail))) {
    } else {
      const int64_t total = expr_text.length() + 1 + tail.length();
      char *buf = static_cast<char *>(allocator.alloc(total + 1));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (expr_text.length() > 0) {
          MEMCPY(buf, expr_text.ptr(), expr_text.length());
        }
        buf[expr_text.length()] = '\n';
        MEMCPY(buf + expr_text.length() + 1, tail.ptr(), tail.length());
        buf[total] = '\0';
        normalized_expr.assign_ptr(buf, total);
      }
    }
  }
  return ret;
}

int ObAiColumnResolver::check_no_ai_dependency(common::ObIAllocator &allocator,
                                                ParseNode *ai_expr,
                                                const common::ObIArray<common::ObString> &ai_column_names)
{
  int ret = OB_SUCCESS;
  if (ai_column_names.count() <= 0) {
    // no AI column to depend on
  } else {
    common::ObSEArray<common::ObString, 8> refs;
    if (OB_FAIL(collect_column_refs(allocator, ai_expr, refs))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < refs.count(); ++i) {
        for (int64_t j = 0; OB_SUCC(ret) && j < ai_column_names.count(); ++j) {
          if (0 == refs.at(i).compare(ai_column_names.at(j))) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN cannot depend on another AI column");
            break;
          }
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
