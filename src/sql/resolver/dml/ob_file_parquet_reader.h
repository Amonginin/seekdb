/*
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
#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_array.h"

namespace oceanbase
{
namespace sql
{

// 一个 parquet 列的原始取值。int64 型与 double 型都填 num_，字符串填 str_。
struct ObParquetValue
{
  bool is_null_;
  bool is_double_;          // true: num_ 当 double 解释；false: num_ 当 int64 解释
  int64_t num_;
  common::ObString str_;    // BYTE_ARRAY / UTF8 时的文本
};

struct ObParquetColumn
{
  common::ObString name_;
  bool nullable_;
  // parquet 物理类型（PT_xxx）。取值转文本时要用它区分 BOOLEAN / INT32 / DOUBLE
  int32_t physical_type_;
  // 该列所有行的取值，按行顺序，长度 = 文件总行数
  common::ObArray<ObParquetValue> *values_;
};

// 解析整份 parquet 文件内容（buf/len 是完整文件字节）：
// 读出 footer 里的 schema（每个叶子列的名字与是否可空）和所有行组的数据页，
// 按列组织成 ObParquetColumn 数组。
// 行数通过 data.row_count() 返回。
class ObParquetFile
{
public:
  explicit ObParquetFile(common::ObIAllocator &alloc);
  ~ObParquetFile();
  int parse(const char *buf, const int64_t len);
  int64_t column_count() const;
  const ObParquetColumn &column(const int64_t idx) const;
  int64_t row_count() const;
private:
  void reset_state();

  common::ObIAllocator &alloc_;
  common::ObArray<ObParquetColumn> columns_;
  int64_t row_count_;
};

} // namespace sql
} // namespace oceanbase
