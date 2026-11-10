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

#pragma once

#include "lib/ob_define.h"
#include "lib/string/ob_sql_string.h"
#include "lib/allocator/ob_allocator.h"
#include "sql/parser/parse_node.h"

namespace oceanbase
{
namespace sql
{

// 把一个 `select ... from read_csv(...)` 语法树翻译成 DESCRIBE 用的 6 列结果 SQL。
// 没有文件表函数时返回 OB_NOT_SUPPORTED。
int ob_file_build_describe_sql_from_select(const ParseNode &select_node,
                                           common::ObIAllocator &alloc,
                                           common::ObSqlString &sql);

} // namespace sql
} // namespace oceanbase
