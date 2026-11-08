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

#ifndef _OB_AI_COLUMN_RESOLVER_H
#define _OB_AI_COLUMN_RESOLVER_H 1

#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_array.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
namespace sql
{

struct ParseNode;

/**
 * Helpers that turn the parsed `AI COLUMN (<expr>) WITH (...)` attribute into
 * the metadata persisted in __all_ai_column.
 *
 * The attribute node carries two children:
 *   children_[0] - the AI expression, parsed as a T_FUN_SYS node whose first
 *                  child is the function name and whose second child is the
 *                  argument list (T_EXPR_LIST).
 *   children_[1] - the optional WITH list, a T_AI_COLUMN_OPTION_LIST node.
 */
class ObAiColumnResolver
{
public:
  ObAiColumnResolver() {}
  ~ObAiColumnResolver() {}

  // The type directive appended to the persisted expression. It tells the
  // worker which shape the model has to answer with.
  static const char *const JSON_TYPE_DIRECTIVE;
  static const char *const INT_TYPE_DIRECTIVE;

  // Parses the AI expression of an AI column attribute.
  //   func_name   - AI_COMPLETE / AI_EMBED, lower case
  //   model_name  - the model id passed as the first argument
  //   expr_text   - the user written expression, rebuilt from the parse tree
  //   dim         - the explicit dimension of AI_EMBED, or OB_INVALID_ID
  static int parse_ai_expr(common::ObIAllocator &allocator,
                            ParseNode *ai_expr,
                            common::ObString &func_name,
                            common::ObString &model_name,
                            common::ObString &expr_text,
                            int64_t &dim);

  // Parses the WITH (...) option list of an AI column attribute.
  static int parse_ai_options(ParseNode *option_node,
                              common::ObString &sync_mode,
                              int64_t &max_retries);

  // Rebuilds a readable expression text out of the parse tree. The result is
  // only meant for storage and diagnostics, not for re-execution.
  static int build_expr_text(common::ObIAllocator &allocator,
                              ParseNode *node,
                              common::ObString &expr_text);

  // Builds the expression that is persisted: the user expression followed by
  // the type directive that matches the column type.
  static int build_normalized_expr(common::ObIAllocator &allocator,
                                    const common::ObString &expr_text,
                                    const ObDataType &column_type,
                                    common::ObString &normalized_expr);

  // Checks that an AI column does not reference another AI column of the same
  // table. Such a definition cannot be evaluated in one pass.
  static int check_no_ai_dependency(common::ObIAllocator &allocator,
                                     ParseNode *ai_expr,
                                     const common::ObIArray<common::ObString> &ai_column_names);
};

} // namespace sql
} // namespace oceanbase

#endif // _OB_AI_COLUMN_RESOLVER_H
