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

#include "ob_file_table_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "lib/ob_errno.h"
#include "lib/utility/alloc_assist.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace sql
{

const char *ob_file_duck_type_name(const ObFileDuckType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case OB_FILE_DUCK_BIGINT: name = "BIGINT"; break;
    case OB_FILE_DUCK_DOUBLE: name = "DOUBLE"; break;
    case OB_FILE_DUCK_BOOLEAN: name = "BOOLEAN"; break;
    case OB_FILE_DUCK_VARCHAR: name = "VARCHAR"; break;
    case OB_FILE_DUCK_DATE: name = "DATE"; break;
    case OB_FILE_DUCK_DATETIME: name = "DATETIME"; break;
    case OB_FILE_DUCK_NULL: name = "VARCHAR"; break;
    default: name = "VARCHAR"; break;
  }
  return name;
}

namespace
{

// 只在本文件用的可增长数组，内存来自语句 allocator。
template <typename T>
struct LocalArray
{
  explicit LocalArray(common::ObIAllocator &alloc)
      : data_(NULL), count_(0), capacity_(0), alloc_(alloc) {}
  int push(const T &value)
  {
    int ret = OB_SUCCESS;
    if (count_ >= capacity_) {
      const int64_t new_capacity = 0 == capacity_ ? 64 : capacity_ * 2;
      T *new_data = static_cast<T *>(alloc_.alloc(sizeof(T) * new_capacity));
      if (NULL == new_data) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (count_ > 0) {
          MEMCPY(new_data, data_, sizeof(T) * count_);
        }
        data_ = new_data;
        capacity_ = new_capacity;
      }
    }
    if (OB_SUCC(ret)) {
      data_[count_++] = value;
    }
    return ret;
  }
  int64_t count() const { return count_; }
  T &at(const int64_t idx) { return data_[idx]; }
  const T &at(const int64_t idx) const { return data_[idx]; }
  T *data_;
  int64_t count_;
  int64_t capacity_;
  common::ObIAllocator &alloc_;
};

bool is_digit(const char c) { return c >= '0' && c <= '9'; }

bool is_space_char(const char c)
{
  return ' ' == c || '\t' == c || '\r' == c || '\n' == c;
}

// 整型：可选正负号 + 全部数字，且不为空
bool looks_like_bigint(const char *ptr, const int64_t len)
{
  bool result = len > 0;
  int64_t i = 0;
  if (result && ('+' == ptr[0] || '-' == ptr[0])) {
    i = 1;
    result = len > 1;
  }
  for (; result && i < len; ++i) {
    if (!is_digit(ptr[i])) {
      result = false;
    }
  }
  return result;
}

// 浮点：可选正负号 + 数字与至多一个小数点 + 可选指数
bool looks_like_double(const char *ptr, const int64_t len)
{
  bool result = len > 0;
  int64_t i = 0;
  int64_t dot_count = 0;
  int64_t digit_count = 0;
  if (result && ('+' == ptr[0] || '-' == ptr[0])) {
    i = 1;
  }
  for (; result && i < len; ++i) {
    if (is_digit(ptr[i])) {
      ++digit_count;
    } else if ('.' == ptr[i]) {
      ++dot_count;
      if (dot_count > 1) {
        result = false;
      }
    } else if ('e' == ptr[i] || 'E' == ptr[i]) {
      // 指数部分必须还有内容
      if (i + 1 >= len) {
        result = false;
      } else {
        ++i;
        if ('+' == ptr[i] || '-' == ptr[i]) {
          ++i;
        }
        for (; result && i < len; ++i) {
          if (!is_digit(ptr[i])) {
            result = false;
          }
        }
      }
    } else {
      result = false;
    }
  }
  return result && digit_count > 0;
}

bool equals_ignore_case(const char *ptr, const int64_t len, const char *word)
{
  bool result = (static_cast<int64_t>(strlen(word)) == len);
  for (int64_t i = 0; result && i < len; ++i) {
    if (tolower(static_cast<unsigned char>(ptr[i])) != tolower(static_cast<unsigned char>(word[i]))) {
      result = false;
    }
  }
  return result;
}

bool looks_like_boolean(const char *ptr, const int64_t len)
{
  return equals_ignore_case(ptr, len, "true") || equals_ignore_case(ptr, len, "false");
}

bool boolean_value(const char *ptr, const int64_t len)
{
  return equals_ignore_case(ptr, len, "true");
}

// YYYY-MM-DD
bool looks_like_date(const char *ptr, const int64_t len)
{
  bool result = (10 == len);
  for (int64_t i = 0; result && i < len; ++i) {
    if (4 == i || 7 == i) {
      result = ('-' == ptr[i]);
    } else {
      result = is_digit(ptr[i]);
    }
  }
  return result;
}

// YYYY-MM-DD[ T]HH:MM:SS
bool looks_like_datetime(const char *ptr, const int64_t len, const char sep)
{
  bool result = (19 == len);
  for (int64_t i = 0; result && i < len; ++i) {
    if (4 == i || 7 == i) {
      result = ('-' == ptr[i]);
    } else if (10 == i) {
      result = (sep == ptr[i]);
    } else if (13 == i || 16 == i) {
      result = (':' == ptr[i]);
    } else {
      result = is_digit(ptr[i]);
    }
  }
  return result;
}

// 简单通配：'*' 任意串，'?' 任意单字符
bool wildcard_match(const char *pattern, const char *text)
{
  bool matched = false;
  if ('\0' == *pattern) {
    matched = ('\0' == *text);
  } else if ('*' == *pattern) {
    matched = wildcard_match(pattern + 1, text) ||
              ('\0' != *text && wildcard_match(pattern, text + 1));
  } else if ('?' == *pattern) {
    matched = ('\0' != *text) && wildcard_match(pattern + 1, text + 1);
  } else {
    matched = (*pattern == *text) && wildcard_match(pattern + 1, text + 1);
  }
  return matched;
}

int append_quoted_string(common::ObSqlString &sql, const char *ptr, const int64_t len)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
    const char c = ptr[i];
    if ('\\' == c) {
      ret = sql.append("\\\\", 2);
    } else if ('\'' == c) {
      ret = sql.append("''", 2);
    } else {
      ret = sql.append(ptr + i, 1);
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("'");
  }
  return ret;
}

int append_quoted_identifier(common::ObSqlString &sql, const common::ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("`"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    const char c = name.ptr()[i];
    if ('`' == c) {
      ret = sql.append("``", 2);
    } else {
      ret = sql.append(name.ptr() + i, 1);
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("`");
  }
  return ret;
}

int append_null_literal(common::ObSqlString &sql, const ObFileDuckType type)
{
  int ret = OB_SUCCESS;
  switch (type) {
    case OB_FILE_DUCK_BIGINT:
    case OB_FILE_DUCK_BOOLEAN:
      ret = sql.append("cast(null as signed)");
      break;
    case OB_FILE_DUCK_DOUBLE:
      ret = sql.append("cast(null as double)");
      break;
    case OB_FILE_DUCK_DATE:
      ret = sql.append("cast(null as date)");
      break;
    case OB_FILE_DUCK_DATETIME:
      ret = sql.append("cast(null as datetime)");
      break;
    default:
      ret = sql.append("cast(null as char(255))");
      break;
  }
  return ret;
}

int append_cell_literal(common::ObSqlString &sql,
                        const ObFileDuckType type,
                        const ObFileCell &cell)
{
  int ret = OB_SUCCESS;
  if (cell.is_null_) {
    ret = append_null_literal(sql, type);
  } else {
    const char *ptr = cell.text_.ptr();
    const int64_t len = cell.text_.length();
    switch (type) {
      case OB_FILE_DUCK_BIGINT: {
        if (!looks_like_bigint(ptr, len)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(sql.append("cast("))) {
        } else if (OB_FAIL(sql.append(ptr, len))) {
        } else {
          ret = sql.append(" as signed)");
        }
        break;
      }
      case OB_FILE_DUCK_DOUBLE: {
        if (!looks_like_double(ptr, len)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(sql.append("cast("))) {
        } else if (OB_FAIL(sql.append(ptr, len))) {
        } else {
          ret = sql.append(" as double)");
        }
        break;
      }
      case OB_FILE_DUCK_BOOLEAN: {
        ret = sql.append(boolean_value(ptr, len) ? "cast(1 as signed)" : "cast(0 as signed)");
        break;
      }
      case OB_FILE_DUCK_DATE: {
        if (OB_FAIL(sql.append("cast("))) {
        } else if (OB_FAIL(append_quoted_string(sql, ptr, len))) {
        } else {
          ret = sql.append(" as date)");
        }
        break;
      }
      case OB_FILE_DUCK_DATETIME: {
        // 把 T 分隔符规范成空格
        char normalized[32];
        int64_t normalized_len = len;
        bool use_normalized = (len <= static_cast<int64_t>(sizeof(normalized)));
        if (use_normalized) {
          MEMCPY(normalized, ptr, len);
          if (len > 10 && 'T' == normalized[10]) {
            normalized[10] = ' ';
          }
        }
        if (OB_FAIL(sql.append("cast("))) {
        } else if (use_normalized
                   && OB_FAIL(append_quoted_string(sql, normalized, normalized_len))) {
        } else if (!use_normalized && OB_FAIL(append_quoted_string(sql, ptr, len))) {
        } else {
          ret = sql.append(" as datetime)");
        }
        break;
      }
      default: {
        ret = append_quoted_string(sql, ptr, len);
        break;
      }
    }
  }
  return ret;
}

} // namespace

ObFileTableData::ObFileTableData(common::ObIAllocator &alloc)
    : alloc_(alloc),
      columns_(NULL),
      column_count_(0),
      column_capacity_(0),
      cells_(NULL),
      cell_count_(0),
      cell_capacity_(0),
      pending_row_cells_(0)
{
}

ObFileTableData::~ObFileTableData()
{
}

int ObFileTableData::copy_string(const char *ptr, const int64_t len, common::ObString &out)
{
  int ret = OB_SUCCESS;
  if (len < 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == len) {
    out.assign_ptr(NULL, 0);
  } else {
    char *dst = static_cast<char *>(alloc_.alloc(len));
    if (NULL == dst) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(dst, ptr, len);
      out.assign_ptr(dst, static_cast<int32_t>(len));
    }
  }
  return ret;
}

int ObFileTableData::reserve_columns(const int64_t need)
{
  int ret = OB_SUCCESS;
  if (need > column_capacity_) {
    const int64_t new_capacity = 0 == column_capacity_ ? 16 : column_capacity_ * 2;
    const int64_t final_capacity = new_capacity > need ? new_capacity : need;
    ObFileColumnMeta *new_columns =
        static_cast<ObFileColumnMeta *>(alloc_.alloc(sizeof(ObFileColumnMeta) * final_capacity));
    if (NULL == new_columns) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (column_count_ > 0) {
        MEMCPY(new_columns, columns_, sizeof(ObFileColumnMeta) * column_count_);
      }
      columns_ = new_columns;
      column_capacity_ = final_capacity;
    }
  }
  return ret;
}

int ObFileTableData::reserve_cells(const int64_t need)
{
  int ret = OB_SUCCESS;
  if (need > cell_capacity_) {
    const int64_t new_capacity = 0 == cell_capacity_ ? 256 : cell_capacity_ * 2;
    const int64_t final_capacity = new_capacity > need ? new_capacity : need;
    ObFileCell *new_cells =
        static_cast<ObFileCell *>(alloc_.alloc(sizeof(ObFileCell) * final_capacity));
    if (NULL == new_cells) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (cell_count_ > 0) {
        MEMCPY(new_cells, cells_, sizeof(ObFileCell) * cell_count_);
      }
      cells_ = new_cells;
      cell_capacity_ = final_capacity;
    }
  }
  return ret;
}

int ObFileTableData::add_column(const common::ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(reserve_columns(column_count_ + 1))) {
  } else {
    ObFileColumnMeta &meta = columns_[column_count_];
    meta.name_ = name;
    meta.type_ = OB_FILE_DUCK_VARCHAR;
    meta.nullable_ = false;
    meta.type_fixed_ = false;
    ++column_count_;
  }
  return ret;
}

int ObFileTableData::finish_columns()
{
  int ret = OB_SUCCESS;
  if (0 == column_count_) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < column_count_; ++i) {
    const common::ObString current = columns_[i].name_;
    int64_t duplicate_count = 0;
    for (int64_t j = 0; j < i; ++j) {
      // 已经改过名的列不再参与比较，所以这里只和「原始名」比：
      // 原始名保存在 original 里，见下面的快照
      if (columns_[j].name_.length() == current.length()
          && 0 == MEMCMP(columns_[j].name_.ptr(), current.ptr(), current.length())) {
        ++duplicate_count;
      }
    }
    if (duplicate_count > 0) {
      char suffix[32];
      const int64_t suffix_len = snprintf(suffix, sizeof(suffix), "__%ld", duplicate_count + 1);
      const int64_t new_len = current.length() + suffix_len;
      char *dst = static_cast<char *>(alloc_.alloc(new_len));
      if (NULL == dst) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(dst, current.ptr(), current.length());
        MEMCPY(dst + current.length(), suffix, suffix_len);
        columns_[i].name_.assign_ptr(dst, static_cast<int32_t>(new_len));
      }
    } else if (current.empty()) {
      char auto_name[32];
      const int64_t auto_len = snprintf(auto_name, sizeof(auto_name), "column%ld", i);
      common::ObString new_name;
      if (OB_FAIL(copy_string(auto_name, auto_len, new_name))) {
      } else {
        columns_[i].name_ = new_name;
      }
    }
  }
  return ret;
}

int ObFileTableData::add_cell(const char *ptr, const int64_t len, const bool is_null)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(reserve_cells(cell_count_ + 1))) {
  } else {
    ObFileCell &cell = cells_[cell_count_];
    cell.is_null_ = is_null;
    if (is_null) {
      cell.text_.assign_ptr(NULL, 0);
    } else if (OB_FAIL(copy_string(ptr, len, cell.text_))) {
    }
    if (OB_SUCC(ret)) {
      ++cell_count_;
      ++pending_row_cells_;
    }
  }
  return ret;
}

int ObFileTableData::end_row()
{
  int ret = OB_SUCCESS;
  if (column_count_ <= 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (0 == pending_row_cells_) {
    // 尾部的空行，忽略
  } else if (pending_row_cells_ != column_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file row column count mismatch", K_(pending_row_cells), K_(column_count));
  } else {
    pending_row_cells_ = 0;
  }
  return ret;
}

int ObFileTableData::set_column_type(const int64_t idx,
                                     const ObFileDuckType type,
                                     const bool nullable)
{
  int ret = OB_SUCCESS;
  if (idx < 0 || idx >= column_count_) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    columns_[idx].type_ = type;
    columns_[idx].nullable_ = nullable;
    columns_[idx].type_fixed_ = true;
  }
  return ret;
}

int ObFileTableData::infer_types()
{
  int ret = OB_SUCCESS;
  if (0 == column_count_) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t col = 0; OB_SUCC(ret) && col < column_count_; ++col) {
    ObFileColumnMeta &meta = columns_[col];
    if (meta.type_fixed_) {
      continue;
    }
    bool all_null = true;
    bool bigint_ok = true;
    bool double_ok = true;
    bool boolean_ok = true;
    bool date_ok = true;
    bool datetime_ok = true;
    bool nullable = false;
    const int64_t rows = row_count();
    for (int64_t row = 0; row < rows; ++row) {
      const ObFileCell &cell = this->cell(row, col);
      if (cell.is_null_) {
        nullable = true;
      } else {
        const char *ptr = cell.text_.ptr();
        const int64_t len = cell.text_.length();
        all_null = false;
        bigint_ok = bigint_ok && looks_like_bigint(ptr, len);
        double_ok = double_ok && looks_like_double(ptr, len);
        boolean_ok = boolean_ok && looks_like_boolean(ptr, len);
        date_ok = date_ok && looks_like_date(ptr, len);
        datetime_ok = datetime_ok &&
            (looks_like_datetime(ptr, len, ' ') || looks_like_datetime(ptr, len, 'T'));
      }
    }
    if (all_null) {
      meta.type_ = OB_FILE_DUCK_VARCHAR;
    } else if (bigint_ok) {
      meta.type_ = OB_FILE_DUCK_BIGINT;
    } else if (double_ok) {
      meta.type_ = OB_FILE_DUCK_DOUBLE;
    } else if (boolean_ok) {
      meta.type_ = OB_FILE_DUCK_BOOLEAN;
    } else if (date_ok) {
      meta.type_ = OB_FILE_DUCK_DATE;
    } else if (datetime_ok) {
      meta.type_ = OB_FILE_DUCK_DATETIME;
    } else {
      meta.type_ = OB_FILE_DUCK_VARCHAR;
    }
    meta.nullable_ = nullable;
  }
  return ret;
}

int ob_file_read_all(const common::ObString &path,
                     common::ObIAllocator &alloc,
                     char *&buf,
                     int64_t &len)
{
  int ret = OB_SUCCESS;
  buf = NULL;
  len = 0;
  char name_buf[1024];
  FILE *fp = NULL;
  if (path.empty() || path.length() >= static_cast<int32_t>(sizeof(name_buf))) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(name_buf, path.ptr(), path.length());
    name_buf[path.length()] = '\0';
    if (NULL == (fp = fopen(name_buf, "rb"))) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("file not found", K(path));
    }
  }
  if (OB_SUCC(ret)) {
    if (0 != fseek(fp, 0, SEEK_END)) {
      ret = OB_IO_ERROR;
    } else {
      const long file_size = ftell(fp);
      if (file_size < 0) {
        ret = OB_IO_ERROR;
      } else if (file_size > 64L * 1024L * 1024L) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("file too large", K(file_size));
      } else if (0 != fseek(fp, 0, SEEK_SET)) {
        ret = OB_IO_ERROR;
      } else {
        len = static_cast<int64_t>(file_size);
        buf = static_cast<char *>(alloc.alloc(len + 1));
        if (NULL == buf) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (len > 0 && static_cast<int64_t>(fread(buf, 1, len, fp)) != len) {
          ret = OB_IO_ERROR;
        } else {
          buf[len] = '\0';
        }
      }
    }
  }
  if (NULL != fp) {
    fclose(fp);
    fp = NULL;
  }
  return ret;
}

namespace
{

struct CsvRawCell
{
  const char *ptr_;
  int64_t len_;
  bool is_null_;
  bool quoted_;
};

int csv_finish_field(const char *scratch,
                     const int64_t field_start,
                     const int64_t field_end,
                     const bool quoted,
                     LocalArray<CsvRawCell> &raw_cells)
{
  CsvRawCell cell;
  cell.ptr_ = scratch + field_start;
  cell.len_ = field_end - field_start;
  // 未加引号的空字段按 NULL 处理，加引号的空串是空字符串
  cell.is_null_ = (!quoted && 0 == cell.len_);
  cell.quoted_ = quoted;
  return raw_cells.push(cell);
}

} // namespace

int ob_file_parse_csv(const char *buf,
                      const int64_t len,
                      const bool has_header,
                      common::ObIAllocator &alloc,
                      ObFileTableData &data)
{
  int ret = OB_SUCCESS;
  if (NULL == buf || len < 0) {
    ret = OB_INVALID_ARGUMENT;
  }

  LocalArray<CsvRawCell> raw_cells(alloc);
  LocalArray<int64_t> row_ends(alloc);  // 每行结束后的累计单元格数
  char *scratch = NULL;
  int64_t scratch_pos = 0;
  int64_t field_start = 0;
  int64_t field_cells = 0;

  if (OB_SUCC(ret)) {
    scratch = static_cast<char *>(alloc.alloc(len + 1));
    if (NULL == scratch) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }

  if (OB_SUCC(ret)) {
    bool in_quotes = false;
    bool after_quote = false;
    bool field_quoted = false;
    int64_t i = 0;
    while (OB_SUCC(ret) && i < len) {
      const char c = buf[i];
      if (in_quotes) {
        if ('"' == c) {
          in_quotes = false;
          after_quote = true;
          ++i;
        } else if ('\r' == c && i + 1 < len && '\n' == buf[i + 1]) {
          // 引号内的 CRLF 归一化成 LF（期望输出里没有 \r）
          scratch[scratch_pos++] = '\n';
          i += 2;
        } else {
          scratch[scratch_pos++] = c;
          ++i;
        }
      } else if (after_quote) {
        if ('"' == c) {
          // "" 转义成一个引号
          scratch[scratch_pos++] = '"';
          in_quotes = true;
          after_quote = false;
          ++i;
        } else if (',' == c) {
          if (OB_FAIL(csv_finish_field(scratch, field_start, scratch_pos, field_quoted, raw_cells))) {
          } else {
            field_start = scratch_pos;
            field_quoted = false;
            after_quote = false;
            ++field_cells;
            ++i;
          }
        } else if ('\r' == c || '\n' == c) {
          if (OB_FAIL(csv_finish_field(scratch, field_start, scratch_pos, field_quoted, raw_cells))) {
          } else {
            field_start = scratch_pos;
            field_quoted = false;
            after_quote = false;
            ++field_cells;
            if (OB_FAIL(row_ends.push(raw_cells.count()))) {
            } else {
              field_cells = 0;
            }
            if ('\r' == c && i + 1 < len && '\n' == buf[i + 1]) {
              i += 2;
            } else {
              ++i;
            }
          }
        } else {
          scratch[scratch_pos++] = c;
          after_quote = false;
          ++i;
        }
      } else {
        if ('"' == c && scratch_pos == field_start) {
          in_quotes = true;
          field_quoted = true;
          ++i;
        } else if (',' == c) {
          if (OB_FAIL(csv_finish_field(scratch, field_start, scratch_pos, field_quoted, raw_cells))) {
          } else {
            field_start = scratch_pos;
            field_quoted = false;
            ++field_cells;
            ++i;
          }
        } else if ('\r' == c || '\n' == c) {
          if (OB_FAIL(csv_finish_field(scratch, field_start, scratch_pos, field_quoted, raw_cells))) {
          } else {
            field_start = scratch_pos;
            field_quoted = false;
            ++field_cells;
            if (OB_FAIL(row_ends.push(raw_cells.count()))) {
            } else {
              field_cells = 0;
            }
            if ('\r' == c && i + 1 < len && '\n' == buf[i + 1]) {
              i += 2;
            } else {
              ++i;
            }
          }
        } else {
          scratch[scratch_pos++] = c;
          ++i;
        }
      }
    }
    // 文件末尾没有换行时补最后一行
    if (OB_SUCC(ret) && 0 != field_cells) {
      if (OB_FAIL(csv_finish_field(scratch, field_start, scratch_pos, field_quoted, raw_cells))) {
      } else if (OB_FAIL(row_ends.push(raw_cells.count()))) {
      } else {
        field_cells = 0;
      }
    }
  }

  // 组装：表头取第一行，其余为数据
  if (OB_SUCC(ret)) {
    if (0 == row_ends.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty csv file");
    }
  }
  if (OB_SUCC(ret)) {
    int64_t data_start_index = 0;
    int64_t column_count = 0;
    if (has_header) {
      column_count = row_ends.at(0);
      data_start_index = row_ends.at(0);
    } else {
      column_count = row_ends.at(0);
      data_start_index = 0;
    }
    if (column_count <= 0 || column_count > 1024) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported csv column count", K(column_count));
    }
    for (int64_t col = 0; OB_SUCC(ret) && col < column_count; ++col) {
      common::ObString column_name;
      if (has_header) {
        const CsvRawCell &cell = raw_cells.at(col);
        if (OB_FAIL(data.copy_string(cell.ptr_, cell.len_, column_name))) {
        }
      } else {
        char auto_name[32];
        const int64_t auto_len = snprintf(auto_name, sizeof(auto_name), "column%ld", col);
        if (OB_FAIL(data.copy_string(auto_name, auto_len, column_name))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(data.add_column(column_name))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(data.finish_columns())) {
    }
    // 逐行灌数据：每行结束后必须调 end_row，否则待定单元格数会累加成 行数×列数
    int64_t cell_index = data_start_index;
    for (int64_t row = (has_header ? 1 : 0); OB_SUCC(ret) && row < row_ends.count(); ++row) {
      const int64_t row_end = row_ends.at(row);
      for (; OB_SUCC(ret) && cell_index < row_end; ++cell_index) {
        const CsvRawCell &cell = raw_cells.at(cell_index);
        if (OB_FAIL(data.add_cell(cell.ptr_, cell.len_, cell.is_null_))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(data.end_row())) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(data.infer_types())) {
    }
  }
  return ret;
}

namespace
{

struct JsonRawCell
{
  const char *key_ptr_;
  int64_t key_len_;
  const char *val_ptr_;
  int64_t val_len_;
  bool is_null_;
};

int json_skip_space(const char *buf, const int64_t len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  while (pos < len && is_space_char(buf[pos])) {
    ++pos;
  }
  return ret;
}

// 解析一个字符串字面量，反序列化到 alloc 分配的缓冲区
int json_parse_string(const char *buf,
                      const int64_t len,
                      int64_t &pos,
                      common::ObIAllocator &alloc,
                      common::ObString &out)
{
  int ret = OB_SUCCESS;
  if (pos >= len || '"' != buf[pos]) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ++pos;
    char *dst = static_cast<char *>(alloc.alloc(len - pos + 1));
    int64_t dst_pos = 0;
    bool closed = false;
    if (NULL == dst) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    while (OB_SUCC(ret) && !closed && pos < len) {
      const char c = buf[pos];
      if ('"' == c) {
        closed = true;
        ++pos;
      } else if ('\\' == c) {
        if (pos + 1 >= len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const char esc = buf[pos + 1];
          char decoded = esc;
          if ('n' == esc) {
            decoded = '\n';
          } else if ('t' == esc) {
            decoded = '\t';
          } else if ('r' == esc) {
            decoded = '\r';
          } else if ('b' == esc) {
            decoded = '\b';
          } else if ('f' == esc) {
            decoded = '\f';
          } else if ('u' == esc) {
            // 只做最小处理：非 ASCII 转义原样保留成 '?'
            decoded = '?';
            pos += 4;
          }
          dst[dst_pos++] = decoded;
          pos += 2;
        }
      } else {
        dst[dst_pos++] = c;
        ++pos;
      }
    }
    if (OB_SUCC(ret)) {
      if (!closed) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        out.assign_ptr(dst, static_cast<int32_t>(dst_pos));
      }
    }
  }
  return ret;
}

// 解析一个标量值（字符串 / 数字 / true / false / null），拒绝对象与数组
int json_parse_scalar(const char *buf,
                      const int64_t len,
                      int64_t &pos,
                      common::ObIAllocator &alloc,
                      JsonRawCell &cell)
{
  int ret = OB_SUCCESS;
  cell.is_null_ = false;
  if (pos >= len) {
    ret = OB_ERR_UNEXPECTED;
  } else if ('"' == buf[pos]) {
    common::ObString text;
    if (OB_FAIL(json_parse_string(buf, len, pos, alloc, text))) {
    } else {
      cell.val_ptr_ = text.ptr();
      cell.val_len_ = text.length();
    }
  } else if ('{' == buf[pos] || '[' == buf[pos]) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("nested json value is not supported");
  } else {
    const int64_t start = pos;
    while (pos < len && ',' != buf[pos] && '}' != buf[pos] && !is_space_char(buf[pos])) {
      ++pos;
    }
    const int64_t value_len = pos - start;
    if (0 == value_len) {
      ret = OB_ERR_UNEXPECTED;
    } else if (equals_ignore_case(buf + start, value_len, "null")) {
      cell.is_null_ = true;
      cell.val_ptr_ = NULL;
      cell.val_len_ = 0;
    } else {
      cell.val_ptr_ = buf + start;
      cell.val_len_ = value_len;
    }
  }
  return ret;
}

} // namespace

int ob_file_parse_jsonl(const char *buf,
                        const int64_t len,
                        common::ObIAllocator &alloc,
                        ObFileTableData &data)
{
  int ret = OB_SUCCESS;
  if (NULL == buf || len < 0) {
    ret = OB_INVALID_ARGUMENT;
  }

  LocalArray<JsonRawCell> cells(alloc);
  LocalArray<int64_t> row_ends(alloc);
  LocalArray<common::ObString> keys(alloc);

  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < len) {
    // 跳过空行
    while (pos < len && (is_space_char(buf[pos]))) {
      ++pos;
    }
    if (pos >= len) {
      break;
    }
    if ('{' != buf[pos]) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("json line must start with {");
    } else {
      ++pos;
      json_skip_space(buf, len, pos);
      if (pos < len && '}' == buf[pos]) {
        ++pos;
      } else if (pos < len) {
        while (OB_SUCC(ret)) {
          JsonRawCell cell;
          common::ObString key;
          json_skip_space(buf, len, pos);
          if (OB_FAIL(json_parse_string(buf, len, pos, alloc, key))) {
          } else {
            cell.key_ptr_ = key.ptr();
            cell.key_len_ = key.length();
            json_skip_space(buf, len, pos);
            if (pos >= len || ':' != buf[pos]) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              ++pos;
              json_skip_space(buf, len, pos);
              if (OB_FAIL(json_parse_scalar(buf, len, pos, alloc, cell))) {
              } else if (OB_FAIL(cells.push(cell))) {
              }
            }
          }
          if (OB_SUCC(ret)) {
            json_skip_space(buf, len, pos);
            if (pos < len && ',' == buf[pos]) {
              ++pos;
            } else if (pos < len && '}' == buf[pos]) {
              ++pos;
              break;
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        }
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(row_ends.push(cells.count()))) {
    }
  }

  // 字段并集，按首次出现顺序
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < cells.count(); ++i) {
      bool found = false;
      for (int64_t k = 0; k < keys.count(); ++k) {
        if (keys.at(k).length() == static_cast<int32_t>(cells.at(i).key_len_)
            && 0 == MEMCMP(keys.at(k).ptr(), cells.at(i).key_ptr_, cells.at(i).key_len_)) {
          found = true;
          break;
        }
      }
      if (!found) {
        common::ObString key;
        if (OB_FAIL(data.copy_string(cells.at(i).key_ptr_, cells.at(i).key_len_, key))) {
        } else if (OB_FAIL(keys.push(key))) {
        }
      }
    }
  }

  if (OB_SUCC(ret)) {
    if (0 == keys.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty jsonl file");
    }
    for (int64_t k = 0; OB_SUCC(ret) && k < keys.count(); ++k) {
      if (OB_FAIL(data.add_column(keys.at(k)))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(data.finish_columns())) {
    }
  }

  // 按行填充，缺失补 NULL
  if (OB_SUCC(ret)) {
    int64_t row_start = 0;
    for (int64_t r = 0; OB_SUCC(ret) && r < row_ends.count(); ++r) {
      const int64_t row_end = row_ends.at(r);
      for (int64_t k = 0; OB_SUCC(ret) && k < keys.count(); ++k) {
        bool found = false;
        for (int64_t i = row_start; !found && i < row_end; ++i) {
          const JsonRawCell &cell = cells.at(i);
          if (keys.at(k).length() == static_cast<int32_t>(cell.key_len_)
              && 0 == MEMCMP(keys.at(k).ptr(), cell.key_ptr_, cell.key_len_)) {
            found = true;
            if (OB_FAIL(data.add_cell(cell.val_ptr_, cell.val_len_, cell.is_null_))) {
            }
          }
        }
        if (OB_SUCC(ret) && !found) {
          if (OB_FAIL(data.add_cell(NULL, 0, true))) {
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(data.end_row())) {
      }
      row_start = row_end;
    }
    if (OB_SUCC(ret) && OB_FAIL(data.infer_types())) {
    }
  }
  return ret;
}

int ob_file_glob(const common::ObString &pattern,
                 common::ObIAllocator &alloc,
                 ObFileTableData &data)
{
  int ret = OB_SUCCESS;
  char pattern_buf[1024];
  char directory[1024];
  const char *file_pattern = NULL;
  DIR *dir = NULL;
  if (pattern.empty() || pattern.length() >= static_cast<int32_t>(sizeof(pattern_buf))) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(pattern_buf, pattern.ptr(), pattern.length());
    pattern_buf[pattern.length()] = '\0';
    // 以最后一个 '/' 切分目录与文件名模式
    const char *last_slash = strrchr(pattern_buf, '/');
    if (NULL == last_slash) {
      snprintf(directory, sizeof(directory), ".");
      file_pattern = pattern_buf;
    } else if (last_slash == pattern_buf) {
      snprintf(directory, sizeof(directory), "/");
      file_pattern = last_slash + 1;
    } else {
      const int64_t dir_len = last_slash - pattern_buf;
      MEMCPY(directory, pattern_buf, dir_len);
      directory[dir_len] = '\0';
      file_pattern = last_slash + 1;
    }
    common::ObString column_name;
    if (OB_FAIL(data.copy_string("file", 4, column_name))) {
    } else if (OB_FAIL(data.add_column(column_name))) {
    } else if (OB_FAIL(data.finish_columns())) {
    }
  }

  if (OB_SUCC(ret)) {
    dir = opendir(directory);
    if (NULL == dir) {
      ret = OB_IO_ERROR;
      LOG_WARN("cannot open directory", K(pattern));
    }
  }
  if (OB_SUCC(ret)) {
    struct dirent *entry = NULL;
    while (OB_SUCC(ret) && NULL != (entry = readdir(dir))) {
      const char *name = entry->d_name;
      if (0 == strcmp(name, ".") || 0 == strcmp(name, "..")) {
        continue;
      }
      if (!wildcard_match(file_pattern, name)) {
        continue;
      }
      char full_path[2048];
      const int64_t full_len = snprintf(full_path, sizeof(full_path), "%s/%s", directory, name);
      if (full_len <= 0 || full_len >= static_cast<int64_t>(sizeof(full_path))) {
        continue;
      }
      if (OB_FAIL(data.add_cell(full_path, full_len, false))) {
      } else if (OB_FAIL(data.end_row())) {
      }
    }
  }
  if (NULL != dir) {
    closedir(dir);
    dir = NULL;
  }
  if (OB_SUCC(ret) && OB_FAIL(data.infer_types())) {
  }
  return ret;
}

int ob_file_build_select_sql(const ObFileTableData &data, common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const int64_t column_count = data.column_count();
  const int64_t row_count = data.row_count();
  if (column_count <= 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (row_count > 100000) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("file has too many rows to materialize", K(row_count));
  } else if (0 == row_count) {
    // 空表：用 where 1 = 0 拿到正确的列定义
    if (OB_FAIL(sql.append("select "))) {
    }
    for (int64_t col = 0; OB_SUCC(ret) && col < column_count; ++col) {
      if (col > 0 && OB_FAIL(sql.append(", "))) {
      }
      if (OB_SUCC(ret) && OB_FAIL(append_null_literal(sql, data.column(col).type_))) {
      }
      if (OB_SUCC(ret) && OB_FAIL(sql.append(" as "))) {
      }
      if (OB_SUCC(ret) && OB_FAIL(append_quoted_identifier(sql, data.column(col).name_))) {
      }
    }
    if (OB_SUCC(ret)) {
      ret = sql.append(" from dual where 1 = 0");
    }
  } else {
    for (int64_t row = 0; OB_SUCC(ret) && row < row_count; ++row) {
      if (row > 0 && OB_FAIL(sql.append(" union all "))) {
      }
      if (OB_SUCC(ret) && OB_FAIL(sql.append("select "))) {
      }
      for (int64_t col = 0; OB_SUCC(ret) && col < column_count; ++col) {
        if (col > 0 && OB_FAIL(sql.append(", "))) {
        }
        if (OB_SUCC(ret)
            && OB_FAIL(append_cell_literal(sql, data.column(col).type_, data.cell(row, col)))) {
        }
        if (OB_SUCC(ret) && OB_FAIL(sql.append(" as "))) {
        }
        if (OB_SUCC(ret) && OB_FAIL(append_quoted_identifier(sql, data.column(col).name_))) {
        }
      }
    }
  }
  return ret;
}

int ob_file_build_describe_sql(const ObFileTableData &data, common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const int64_t column_count = data.column_count();
  if (column_count <= 0) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t col = 0; OB_SUCC(ret) && col < column_count; ++col) {
    const ObFileColumnMeta &meta = data.column(col);
    if (col > 0 && OB_FAIL(sql.append(" union all "))) {
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append("select "))) {
    }
    if (OB_SUCC(ret) && OB_FAIL(append_quoted_string(sql, meta.name_.ptr(), meta.name_.length()))) {
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append(" as column_name, "))) {
    }
    if (OB_SUCC(ret)) {
      const char *type_name = ob_file_duck_type_name(meta.type_);
      if (OB_FAIL(append_quoted_string(sql, type_name, static_cast<int64_t>(strlen(type_name))))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append(" as column_type, "))) {
    }
    if (OB_SUCC(ret)) {
      const char *nullable = meta.nullable_ ? "YES" : "NO";
      if (OB_FAIL(append_quoted_string(sql, nullable, static_cast<int64_t>(strlen(nullable))))) {
      }
    }
    if (OB_SUCC(ret)) {
      ret = sql.append(" as `null`, null as `key`, null as `default`, null as `extra`");
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
