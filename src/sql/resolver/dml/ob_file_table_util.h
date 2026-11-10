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
#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase
{
namespace sql
{

// DuckDB 风格的列类型。文件和 DESCRIBE 都用这套拼写，不能换成 OB 的 bigint(20)。
enum ObFileDuckType
{
  OB_FILE_DUCK_NULL = 0,
  OB_FILE_DUCK_BIGINT = 1,
  OB_FILE_DUCK_DOUBLE = 2,
  OB_FILE_DUCK_BOOLEAN = 3,
  OB_FILE_DUCK_VARCHAR = 4,
  OB_FILE_DUCK_DATE = 5,
  OB_FILE_DUCK_DATETIME = 6,
  OB_FILE_DUCK_MAX
};

const char *ob_file_duck_type_name(const ObFileDuckType type);

struct ObFileColumnMeta
{
  common::ObString name_;
  ObFileDuckType type_;
  // 该列是否出现过 NULL/空值，DESCRIBE 的 null 列直接取它
  bool nullable_;
  // 文件格式自带类型（parquet）时为 true，跳过文本推断
  bool type_fixed_;
};

struct ObFileCell
{
  common::ObString text_;
  bool is_null_;
};

// 物化后的整份文件：列元信息 + 扁平的单元格数组（下标 row * column_count + col）。
// 所有内存都从传入的 allocator 分配，生命周期跟随语句。
class ObFileTableData
{
public:
  explicit ObFileTableData(common::ObIAllocator &alloc);
  ~ObFileTableData();

  int add_column(const common::ObString &name);
  int finish_columns();
  int add_cell(const char *ptr, const int64_t len, const bool is_null);
  int end_row();
  int set_column_type(const int64_t idx, const ObFileDuckType type, const bool nullable);
  int infer_types();

  int64_t column_count() const { return column_count_; }
  int64_t row_count() const { return column_count_ <= 0 ? 0 : cell_count_ / column_count_; }
  const ObFileColumnMeta &column(const int64_t idx) const { return columns_[idx]; }
  const ObFileCell &cell(const int64_t row, const int64_t col) const
  {
    return cells_[row * column_count_ + col];
  }
  common::ObIAllocator &allocator() { return alloc_; }
  int copy_string(const char *ptr, const int64_t len, common::ObString &out);

private:
  int reserve_columns(const int64_t need);
  int reserve_cells(const int64_t need);

private:
  common::ObIAllocator &alloc_;
  ObFileColumnMeta *columns_;
  int64_t column_count_;
  int64_t column_capacity_;
  ObFileCell *cells_;
  int64_t cell_count_;
  int64_t cell_capacity_;
  int64_t pending_row_cells_;
};

// 把整个文件读进内存。返回的 buf 由 allocator 分配，不保证以 '\0' 结尾。
int ob_file_read_all(const common::ObString &path,
                     common::ObIAllocator &alloc,
                     char *&buf,
                     int64_t &len);

// CSV：支持引号包裹、"" 转义、字段内换行（CRLF 归一化成 LF）、CRLF 行结束、空字段当 NULL。
int ob_file_parse_csv(const char *buf,
                      const int64_t len,
                      const bool has_header,
                      common::ObIAllocator &alloc,
                      ObFileTableData &data);

// JSONL：每行一个扁平 JSON 对象，字段取并集，缺失补 NULL。
int ob_file_parse_jsonl(const char *buf,
                        const int64_t len,
                        common::ObIAllocator &alloc,
                        ObFileTableData &data);

// glob：单列 file，值为完整路径。
int ob_file_glob(const common::ObString &pattern,
                 common::ObIAllocator &alloc,
                 ObFileTableData &data);

// 把物化数据拼成 "select ... union all select ..." 的 SQL 文本。
int ob_file_build_select_sql(const ObFileTableData &data, common::ObSqlString &sql);

// 拼 DESCRIBE 的 6 列结果（column_name / column_type / null / key / default / extra）。
int ob_file_build_describe_sql(const ObFileTableData &data, common::ObSqlString &sql);

} // namespace sql
} // namespace oceanbase
