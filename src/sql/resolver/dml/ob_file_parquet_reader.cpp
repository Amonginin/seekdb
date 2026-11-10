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

#include "sql/resolver/dml/ob_file_parquet_reader.h"

#define USING_LOG_PREFIX SQL_RESV

#include <new>
#include "lib/allocator/page_arena.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{

const char PARQUET_MAGIC[4] = {'P', 'A', 'R', '1'};
const int64_t PARQUET_FOOTER_SIZE = 8;
const int64_t PARQUET_MIN_FILE_SIZE = 12;

// ---------------------------------------------------------------------------
// thrift compact protocol 的 wire type
// ---------------------------------------------------------------------------
enum ParquetCompactType : int32_t
{
  CT_STOP = 0,
  CT_BOOLEAN_TRUE = 1,
  CT_BOOLEAN_FALSE = 2,
  CT_BYTE = 3,
  CT_I16 = 4,
  CT_I32 = 5,
  CT_I64 = 6,
  CT_DOUBLE = 7,
  CT_BINARY = 8,
  CT_LIST = 9,
  CT_SET = 10,
  CT_MAP = 11,
  CT_STRUCT = 12
};

// parquet 物理类型
enum ParquetPhysicalType : int32_t
{
  PT_BOOLEAN = 0,
  PT_INT32 = 1,
  PT_INT64 = 2,
  PT_INT96 = 3,
  PT_FLOAT = 4,
  PT_DOUBLE = 5,
  PT_BYTE_ARRAY = 6,
  PT_FIXED_LEN_BYTE_ARRAY = 7
};

// parquet repetition type
enum ParquetRepetitionType : int32_t
{
  RT_REQUIRED = 0,
  RT_OPTIONAL = 1,
  RT_REPEATED = 2
};

// PageType
enum ParquetPageType : int32_t
{
  PAGE_TYPE_DATA = 0,
  PAGE_TYPE_INDEX = 1,
  PAGE_TYPE_DICTIONARY = 2,
  PAGE_TYPE_DATA_V2 = 3
};

// Encoding
enum ParquetEncoding : int32_t
{
  ENCODING_PLAIN = 0
};

// Compression codec
enum ParquetCodec : int32_t
{
  CODEC_UNCOMPRESSED = 0,
  CODEC_SNAPPY = 1
};

inline uint32_t read_le_u32(const char *p)
{
  return static_cast<uint32_t>(static_cast<uint8_t>(p[0]))
         | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8)
         | (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16)
         | (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

inline uint64_t read_le_u64(const char *p)
{
  return static_cast<uint64_t>(read_le_u32(p))
         | (static_cast<uint64_t>(read_le_u32(p + 4)) << 32);
}

inline int32_t bit_width_of(const int32_t max_value)
{
  int32_t width = 0;
  while (width < 32 && (static_cast<int64_t>(1) << width) <= static_cast<int64_t>(max_value)) {
    ++width;
  }
  return width;
}

// ---------------------------------------------------------------------------
// raw snappy 解压（parquet 的 SNAPPY codec 就是 raw 块格式）
// 格式：varint 解压后长度 + 若干 tag(literal / copy1 / copy2 / copy4)
// ---------------------------------------------------------------------------
int snappy_decompress(const char *src, const int64_t src_len, char *dst, const int64_t dst_len)
{
  int ret = OB_SUCCESS;
  int64_t src_pos = 0;
  int64_t dst_pos = 0;
  uint64_t decoded_len = 0;
  int32_t shift = 0;
  bool len_done = false;
  while (OB_SUCC(ret) && !len_done) {
    if (src_pos >= src_len || shift > 63) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const uint8_t byte = static_cast<uint8_t>(src[src_pos]);
      ++src_pos;
      decoded_len |= (static_cast<uint64_t>(byte) & 0x7F) << shift;
      shift += 7;
      len_done = (0 == (byte & 0x80));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (static_cast<int64_t>(decoded_len) > dst_len) {
    ret = OB_ERR_UNEXPECTED;
  }
  // 逐 tag 解码，直到输出够 decoded_len
  while (OB_SUCC(ret) && dst_pos < static_cast<int64_t>(decoded_len)) {
    if (src_pos >= src_len) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const uint8_t tag = static_cast<uint8_t>(src[src_pos]);
      ++src_pos;
      const int32_t type = tag & 0x03;
      if (0 == type) {
        // literal：长度写在 tag 里，>=60 时跟 1~4 字节小端长度
        int64_t lit_len = (tag >> 2);
        if (lit_len < 60) {
          ++lit_len;
        } else {
          const int32_t extra_bytes = static_cast<int32_t>(lit_len - 59);
          if (extra_bytes < 1 || extra_bytes > 4 || src_pos + extra_bytes > src_len) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            uint32_t len_bytes = 0;
            for (int32_t i = 0; i < extra_bytes; ++i) {
              len_bytes |= static_cast<uint32_t>(static_cast<uint8_t>(src[src_pos + i])) << (8 * i);
            }
            src_pos += extra_bytes;
            lit_len = static_cast<int64_t>(len_bytes) + 1;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (src_pos + lit_len > src_len) {
          ret = OB_ERR_UNEXPECTED;
        } else if (dst_pos + lit_len > static_cast<int64_t>(decoded_len)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          MEMCPY(dst + dst_pos, src + src_pos, lit_len);
          src_pos += lit_len;
          dst_pos += lit_len;
        }
      } else {
        // copy：从已输出的数据里回溯复制
        int64_t copy_len = 0;
        int64_t offset = 0;
        if (1 == type) {
          // copy1：3 位长度 + 1 字节低 8 位偏移
          if (src_pos + 1 > src_len) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            copy_len = ((tag >> 2) & 0x07) + 4;
            offset = ((static_cast<int64_t>(tag) & 0xE0) << 3)
                     | static_cast<uint8_t>(src[src_pos]);
            src_pos += 1;
          }
        } else if (2 == type) {
          if (src_pos + 2 > src_len) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            copy_len = (tag >> 2) + 1;
            offset = static_cast<uint8_t>(src[src_pos])
                     | (static_cast<int64_t>(static_cast<uint8_t>(src[src_pos + 1])) << 8);
            src_pos += 2;
          }
        } else {
          if (src_pos + 4 > src_len) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            copy_len = (tag >> 2) + 1;
            offset = static_cast<uint8_t>(src[src_pos])
                     | (static_cast<int64_t>(static_cast<uint8_t>(src[src_pos + 1])) << 8)
                     | (static_cast<int64_t>(static_cast<uint8_t>(src[src_pos + 2])) << 16)
                     | (static_cast<int64_t>(static_cast<uint8_t>(src[src_pos + 3])) << 24);
            src_pos += 4;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (offset <= 0 || offset > dst_pos) {
          ret = OB_ERR_UNEXPECTED;
        } else if (dst_pos + copy_len > static_cast<int64_t>(decoded_len)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          // 允许重叠，必须逐字节
          for (int64_t i = 0; i < copy_len; ++i) {
            dst[dst_pos] = dst[dst_pos - offset];
            ++dst_pos;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && dst_pos != static_cast<int64_t>(decoded_len)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// 手写的 thrift compact protocol 读取器
// ---------------------------------------------------------------------------
class ParquetCompactReader
{
public:
  ParquetCompactReader() : buf_(NULL), len_(0), pos_(0) {}
  ~ParquetCompactReader() {}

  void reset(const char *buf, const int64_t len, const int64_t pos = 0)
  {
    buf_ = buf;
    len_ = len;
    pos_ = pos;
  }
  int64_t pos() const { return pos_; }
  const char *buf() const { return buf_; }

  int read_byte(uint8_t &val)
  {
    int ret = OB_SUCCESS;
    if (OB_ISNULL(buf_) || pos_ < 0 || pos_ >= len_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      val = static_cast<uint8_t>(buf_[pos_]);
      ++pos_;
    }
    return ret;
  }

  int read_varint(uint64_t &val)
  {
    int ret = OB_SUCCESS;
    val = 0;
    int32_t shift = 0;
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      uint8_t byte = 0;
      if (OB_FAIL(read_byte(byte))) {
      } else if (shift > 63) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        val |= (static_cast<uint64_t>(byte) & 0x7F) << shift;
        shift += 7;
        done = (0 == (byte & 0x80));
      }
    }
    return ret;
  }

  int read_zigzag(int64_t &val)
  {
    int ret = OB_SUCCESS;
    uint64_t raw = 0;
    if (OB_FAIL(read_varint(raw))) {
    } else if (0 == (raw & 1)) {
      val = static_cast<int64_t>(raw >> 1);
    } else {
      val = -static_cast<int64_t>((raw >> 1) + 1);
    }
    return ret;
  }

  int read_i32(int32_t &val)
  {
    int ret = OB_SUCCESS;
    int64_t tmp = 0;
    if (OB_FAIL(read_zigzag(tmp))) {
    } else if (tmp < INT32_MIN || tmp > INT32_MAX) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      val = static_cast<int32_t>(tmp);
    }
    return ret;
  }

  int read_i64(int64_t &val)
  {
    return read_zigzag(val);
  }

  int read_double(double &val)
  {
    int ret = OB_SUCCESS;
    if (pos_ < 0 || pos_ + 8 > len_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const uint64_t bits = read_le_u64(buf_ + pos_);
      MEMCPY(&val, &bits, sizeof(val));
      pos_ += 8;
    }
    return ret;
  }

  // BINARY：varint 长度 + 字节，返回的是指向 buf_ 的切片，不做拷贝
  int read_binary(ObString &str)
  {
    int ret = OB_SUCCESS;
    uint64_t len = 0;
    if (OB_FAIL(read_varint(len))) {
    } else if (pos_ < 0 || pos_ > len_ || len > static_cast<uint64_t>(len_ - pos_)
               || len > INT32_MAX) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      str.assign_ptr(buf_ + pos_, static_cast<int32_t>(len));
      pos_ += static_cast<int64_t>(len);
    }
    return ret;
  }

  int read_list_header(int32_t &size, int32_t &elem_type)
  {
    int ret = OB_SUCCESS;
    uint8_t header = 0;
    if (OB_FAIL(read_byte(header))) {
    } else {
      size = (header >> 4) & 0x0F;
      elem_type = header & 0x0F;
      if (15 == size) {
        uint64_t real_size = 0;
        if (OB_FAIL(read_varint(real_size))) {
        } else if (real_size > INT32_MAX) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          size = static_cast<int32_t>(real_size);
        }
      }
      if (OB_SUCC(ret) && size < 0) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    return ret;
  }

  // 返回 ctype == CT_STOP 表示 struct 结束
  int read_field_header(int16_t &last_id, int16_t &field_id, int32_t &ctype)
  {
    int ret = OB_SUCCESS;
    uint8_t header = 0;
    field_id = 0;
    ctype = CT_STOP;
    if (OB_FAIL(read_byte(header))) {
    } else if (0 == header) {
      // STOP
    } else {
      const int32_t delta = (header >> 4) & 0x0F;
      ctype = header & 0x0F;
      if (0 == delta) {
        int64_t real_id = 0;
        if (OB_FAIL(read_zigzag(real_id))) {
        } else if (real_id < INT16_MIN || real_id > INT16_MAX) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          field_id = static_cast<int16_t>(real_id);
        }
      } else {
        field_id = static_cast<int16_t>(last_id + delta);
      }
      if (OB_SUCC(ret)) {
        last_id = field_id;
      }
    }
    return ret;
  }

  int skip(const int32_t ctype)
  {
    int ret = OB_SUCCESS;
    switch (ctype) {
      case CT_STOP:
      case CT_BOOLEAN_TRUE:
      case CT_BOOLEAN_FALSE:
        // 布尔值就写在字段头里，没有额外字节
        break;
      case CT_BYTE: {
        uint8_t byte = 0;
        ret = read_byte(byte);
        break;
      }
      case CT_I16:
      case CT_I32:
      case CT_I64: {
        uint64_t tmp = 0;
        ret = read_varint(tmp);
        break;
      }
      case CT_DOUBLE: {
        if (pos_ < 0 || pos_ + 8 > len_) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          pos_ += 8;
        }
        break;
      }
      case CT_BINARY: {
        uint64_t len = 0;
        if (OB_FAIL(read_varint(len))) {
        } else if (pos_ < 0 || pos_ > len_ || len > static_cast<uint64_t>(len_ - pos_)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          pos_ += static_cast<int64_t>(len);
        }
        break;
      }
      case CT_LIST:
      case CT_SET: {
        int32_t size = 0;
        int32_t elem_type = 0;
        if (OB_FAIL(read_list_header(size, elem_type))) {
        } else {
          for (int32_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            ret = skip(elem_type);
          }
        }
        break;
      }
      case CT_MAP: {
        uint64_t size = 0;
        if (OB_FAIL(read_varint(size))) {
        } else if (size > 0) {
          uint8_t kv_type = 0;
          if (OB_FAIL(read_byte(kv_type))) {
          } else {
            const int32_t key_type = (kv_type >> 4) & 0x0F;
            const int32_t value_type = kv_type & 0x0F;
            for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
              if (OB_FAIL(skip(key_type))) {
              } else {
                ret = skip(value_type);
              }
            }
          }
        }
        break;
      }
      case CT_STRUCT: {
        int16_t last_id = 0;
        bool done = false;
        while (OB_SUCC(ret) && !done) {
          int16_t field_id = 0;
          int32_t field_type = 0;
          if (OB_FAIL(read_field_header(last_id, field_id, field_type))) {
          } else if (CT_STOP == field_type) {
            done = true;
          } else {
            ret = skip(field_type);
          }
        }
        break;
      }
      default:
        ret = OB_NOT_SUPPORTED;
        break;
    }
    return ret;
  }

private:
  const char *buf_;
  int64_t len_;
  int64_t pos_;
};

// ---------------------------------------------------------------------------
// RLE / bit-packed hybrid 解码器（用于 definition level）
// ---------------------------------------------------------------------------
class ParquetRleBpDecoder
{
public:
  ParquetRleBpDecoder()
    : buf_(NULL), len_(0), pos_(0), bit_width_(0), rle_value_(0), rle_remaining_(0),
      bp_buf_(NULL), bp_len_(0), bp_total_(0), bp_bit_offset_(0)
  {}
  ~ParquetRleBpDecoder() {}

  int init(const char *buf, const int64_t len, const int32_t bit_width)
  {
    int ret = OB_SUCCESS;
    if (OB_ISNULL(buf) || len < 0 || bit_width <= 0 || bit_width > 32) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      buf_ = buf;
      len_ = len;
      pos_ = 0;
      bit_width_ = bit_width;
      rle_value_ = 0;
      rle_remaining_ = 0;
      bp_buf_ = NULL;
      bp_len_ = 0;
      bp_total_ = 0;
      bp_bit_offset_ = 0;
    }
    return ret;
  }

  // 取下一个值，数据取完返回 OB_ITER_END
  int next_value(int32_t &val)
  {
    int ret = OB_SUCCESS;
    val = 0;
    while (OB_SUCC(ret) && 0 == rle_remaining_ && bp_bit_offset_ >= bp_total_) {
      ret = load_run();
    }
    if (OB_SUCC(ret)) {
      if (rle_remaining_ > 0) {
        val = rle_value_;
        --rle_remaining_;
      } else if (bp_bit_offset_ < bp_total_) {
        if (OB_FAIL(read_bit_packed_value(val))) {
        }
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    return ret;
  }

private:
  int load_run()
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_ITER_END;
    } else {
      uint64_t header = 0;
      int32_t shift = 0;
      bool done = false;
      while (OB_SUCC(ret) && !done) {
        if (pos_ >= len_) {
          ret = OB_ERR_UNEXPECTED;
        } else if (shift > 63) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const uint8_t byte = static_cast<uint8_t>(buf_[pos_]);
          ++pos_;
          header |= (static_cast<uint64_t>(byte) & 0x7F) << shift;
          shift += 7;
          done = (0 == (byte & 0x80));
        }
      }
      if (OB_SUCC(ret)) {
        if (0 == (header & 1)) {
          // RLE run：run_length 个相同的值，值占 ceil(bit_width / 8) 字节
          const int64_t run_length = static_cast<int64_t>(header >> 1);
          const int32_t byte_width = (bit_width_ + 7) / 8;
          if (pos_ + byte_width > len_) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            uint32_t value = 0;
            for (int32_t i = 0; i < byte_width; ++i) {
              value |= static_cast<uint32_t>(static_cast<uint8_t>(buf_[pos_ + i])) << (8 * i);
            }
            pos_ += byte_width;
            rle_value_ = static_cast<int32_t>(value);
            rle_remaining_ = run_length;
          }
        } else {
          // bit-packed run：num_groups 组，每组 8 个值，恰好占 num_groups * bit_width 字节
          const int64_t groups = static_cast<int64_t>(header >> 1);
          if (groups > (len_ - pos_) / bit_width_) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const int64_t total_bytes = groups * bit_width_;
            bp_buf_ = buf_ + pos_;
            bp_len_ = total_bytes;
            bp_total_ = groups * 8 * bit_width_;
            bp_bit_offset_ = 0;
            pos_ += total_bytes;
          }
        }
      }
    }
    return ret;
  }

  int read_bit_packed_value(int32_t &val)
  {
    int ret = OB_SUCCESS;
    const int64_t byte_index = bp_bit_offset_ / 8;
    int32_t shift = static_cast<int32_t>(bp_bit_offset_ % 8);
    int64_t used_bits = 8 - shift;
    uint64_t raw = 0;
    if (byte_index < 0 || byte_index >= bp_len_ || bp_bit_offset_ + bit_width_ > bp_total_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      raw = static_cast<uint64_t>(static_cast<uint8_t>(bp_buf_[byte_index])) >> shift;
      int64_t next_index = byte_index + 1;
      while (used_bits < bit_width_ && next_index < bp_len_) {
        raw |= static_cast<uint64_t>(static_cast<uint8_t>(bp_buf_[next_index])) << used_bits;
        used_bits += 8;
        ++next_index;
      }
      raw &= ((static_cast<uint64_t>(1) << bit_width_) - 1);
      val = static_cast<int32_t>(raw);
      bp_bit_offset_ += bit_width_;
    }
    return ret;
  }

private:
  const char *buf_;
  int64_t len_;
  int64_t pos_;
  int32_t bit_width_;
  int32_t rle_value_;
  int64_t rle_remaining_;
  const char *bp_buf_;
  int64_t bp_len_;
  int64_t bp_total_;
  int64_t bp_bit_offset_;
};

// ---------------------------------------------------------------------------
// PLAIN 编码的取值解码器
// ---------------------------------------------------------------------------
class ParquetPlainDecoder
{
public:
  ParquetPlainDecoder() : buf_(NULL), len_(0), pos_(0), bit_pos_(0), physical_type_(0) {}
  ~ParquetPlainDecoder() {}

  int init(const char *buf, const int64_t len, const int32_t physical_type)
  {
    int ret = OB_SUCCESS;
    if (OB_ISNULL(buf) || len < 0) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      buf_ = buf;
      len_ = len;
      pos_ = 0;
      bit_pos_ = 0;
      physical_type_ = physical_type;
    }
    return ret;
  }

  int next_value(ObIAllocator &alloc, ObParquetValue &val)
  {
    int ret = OB_SUCCESS;
    val.is_null_ = false;
    val.is_double_ = false;
    val.num_ = 0;
    switch (physical_type_) {
      case PT_BOOLEAN: {
        const int64_t byte_index = bit_pos_ / 8;
        if (byte_index < 0 || byte_index >= len_) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const uint8_t byte = static_cast<uint8_t>(buf_[byte_index]);
          val.num_ = (byte >> (bit_pos_ % 8)) & 1;
          ++bit_pos_;
        }
        break;
      }
      case PT_INT32: {
        if (pos_ + 4 > len_) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          val.num_ = static_cast<int64_t>(static_cast<int32_t>(read_le_u32(buf_ + pos_)));
          pos_ += 4;
        }
        break;
      }
      case PT_INT64: {
        if (pos_ + 8 > len_) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          val.num_ = static_cast<int64_t>(read_le_u64(buf_ + pos_));
          pos_ += 8;
        }
        break;
      }
      case PT_FLOAT: {
        if (pos_ + 4 > len_) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const uint32_t bits = read_le_u32(buf_ + pos_);
          pos_ += 4;
          float f = 0.0f;
          MEMCPY(&f, &bits, sizeof(f));
          const double d = static_cast<double>(f);
          MEMCPY(&val.num_, &d, sizeof(d));
          val.is_double_ = true;
        }
        break;
      }
      case PT_DOUBLE: {
        if (pos_ + 8 > len_) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const uint64_t bits = read_le_u64(buf_ + pos_);
          pos_ += 8;
          double d = 0.0;
          MEMCPY(&d, &bits, sizeof(d));
          MEMCPY(&val.num_, &d, sizeof(d));
          val.is_double_ = true;
        }
        break;
      }
      case PT_BYTE_ARRAY: {
        if (pos_ + 4 > len_) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const uint32_t str_len = read_le_u32(buf_ + pos_);
          pos_ += 4;
          if (pos_ + static_cast<int64_t>(str_len) > len_) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            ret = copy_string(alloc, buf_ + pos_, static_cast<int64_t>(str_len), val.str_);
            pos_ += static_cast<int64_t>(str_len);
          }
        }
        break;
      }
      case PT_INT96:
      case PT_FIXED_LEN_BYTE_ARRAY:
      default:
        ret = OB_NOT_SUPPORTED;
        break;
    }
    return ret;
  }

private:
  static int copy_string(ObIAllocator &alloc, const char *src, const int64_t len, ObString &dst)
  {
    int ret = OB_SUCCESS;
    if (len < 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == len) {
      dst.assign_ptr(NULL, 0);
    } else {
      char *buf = static_cast<char *>(alloc.alloc(len));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, src, len);
        dst.assign_ptr(buf, static_cast<int32_t>(len));
      }
    }
    return ret;
  }

private:
  const char *buf_;
  int64_t len_;
  int64_t pos_;
  int64_t bit_pos_;
  int32_t physical_type_;
};

// ---------------------------------------------------------------------------
// 元数据结构
// ---------------------------------------------------------------------------
struct ParquetSchemaElement
{
  ParquetSchemaElement()
    : physical_type_(0), repetition_type_(0), num_children_(0),
      has_physical_type_(false), has_repetition_type_(false), has_num_children_(false)
  {}
  int32_t physical_type_;
  int32_t repetition_type_;
  int32_t num_children_;
  bool has_physical_type_;
  bool has_repetition_type_;
  bool has_num_children_;
  ObString name_;
};

struct ParquetLeafColumn
{
  ParquetLeafColumn() : physical_type_(0), nullable_(false), max_def_level_(0) {}
  ObString name_;
  int32_t physical_type_;
  bool nullable_;
  int32_t max_def_level_;
};

struct ParquetColumnChunk
{
  ParquetColumnChunk()
    : column_idx_(-1), physical_type_(0), codec_(0), num_values_(0),
      data_page_offset_(0), dictionary_page_offset_(0), has_dictionary_page_(false)
  {}
  int32_t column_idx_;
  int32_t physical_type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
  bool has_dictionary_page_;
  ObString path_;
};

struct ParquetRowGroup
{
  ParquetRowGroup() : num_rows_(0), chunk_start_(0), chunk_count_(0) {}
  int64_t num_rows_;
  int64_t chunk_start_;
  int64_t chunk_count_;
};

struct ParquetFileMeta
{
  explicit ParquetFileMeta(ObIAllocator &alloc)
    : schema_(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator(alloc, "ParquetMeta")),
      leaves_(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator(alloc, "ParquetMeta")),
      chunks_(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator(alloc, "ParquetMeta")),
      row_groups_(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator(alloc, "ParquetMeta")),
      num_rows_(0)
  {}
  ObArray<ParquetSchemaElement> schema_;
  ObArray<ParquetLeafColumn> leaves_;
  ObArray<ParquetColumnChunk> chunks_;
  ObArray<ParquetRowGroup> row_groups_;
  int64_t num_rows_;
};

struct ParquetDataPageHeader
{
  ParquetDataPageHeader()
    : num_values_(0), encoding_(0), definition_level_encoding_(0),
      repetition_level_encoding_(0), valid_(false)
  {}
  int32_t num_values_;
  int32_t encoding_;
  int32_t definition_level_encoding_;
  int32_t repetition_level_encoding_;
  bool valid_;
};

struct ParquetDataPageHeaderV2
{
  ParquetDataPageHeaderV2()
    : num_values_(0), num_nulls_(0), num_rows_(0), encoding_(0),
      definition_levels_byte_length_(0), repetition_levels_byte_length_(0),
      is_compressed_(true), valid_(false)
  {}
  int32_t num_values_;
  int32_t num_nulls_;
  int32_t num_rows_;
  int32_t encoding_;
  int32_t definition_levels_byte_length_;
  int32_t repetition_levels_byte_length_;
  bool is_compressed_;
  bool valid_;
};

struct ParquetPageHeader
{
  ParquetPageHeader()
    : type_(0), uncompressed_page_size_(0), compressed_page_size_(0) {}
  int32_t type_;
  int32_t uncompressed_page_size_;
  int32_t compressed_page_size_;
  ParquetDataPageHeader data_page_header_;
  ParquetDataPageHeaderV2 data_page_header_v2_;
};

// ---------------------------------------------------------------------------
// 元数据解析
// ---------------------------------------------------------------------------
int parse_schema_element(ParquetCompactReader &reader, ParquetSchemaElement &element)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    int16_t field_id = 0;
    int32_t ctype = 0;
    if (OB_FAIL(reader.read_field_header(last_id, field_id, ctype))) {
    } else if (CT_STOP == ctype) {
      done = true;
    } else if (1 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(element.physical_type_);
      if (OB_SUCC(ret)) {
        element.has_physical_type_ = true;
      }
    } else if (3 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(element.repetition_type_);
      if (OB_SUCC(ret)) {
        element.has_repetition_type_ = true;
      }
    } else if (4 == field_id && CT_BINARY == ctype) {
      ret = reader.read_binary(element.name_);
    } else if (5 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(element.num_children_);
      if (OB_SUCC(ret)) {
        element.has_num_children_ = true;
      }
    } else {
      ret = reader.skip(ctype);
    }
  }
  return ret;
}

int parse_schema_list(ParquetCompactReader &reader, ParquetFileMeta &meta)
{
  int ret = OB_SUCCESS;
  int32_t list_size = 0;
  int32_t elem_type = 0;
  if (OB_FAIL(reader.read_list_header(list_size, elem_type))) {
  } else if (CT_STRUCT != elem_type) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int32_t i = 0; OB_SUCC(ret) && i < list_size; ++i) {
      ParquetSchemaElement element;
      if (OB_FAIL(parse_schema_element(reader, element))) {
      } else {
        ret = meta.schema_.push_back(element);
      }
    }
  }
  return ret;
}

int parse_column_meta_data(ParquetCompactReader &reader, ParquetColumnChunk &chunk)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    int16_t field_id = 0;
    int32_t ctype = 0;
    if (OB_FAIL(reader.read_field_header(last_id, field_id, ctype))) {
    } else if (CT_STOP == ctype) {
      done = true;
    } else if (1 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(chunk.physical_type_);
    } else if (3 == field_id && CT_LIST == ctype) {
      int32_t path_size = 0;
      int32_t path_type = 0;
      if (OB_FAIL(reader.read_list_header(path_size, path_type))) {
      } else if (CT_BINARY != path_type) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        for (int32_t i = 0; OB_SUCC(ret) && i < path_size; ++i) {
          ObString path_part;
          if (OB_FAIL(reader.read_binary(path_part))) {
          } else if (0 == i) {
            chunk.path_ = path_part;
          } else {
            // 多层路径说明是嵌套 schema，本实现不支持
            ret = OB_NOT_SUPPORTED;
          }
        }
      }
    } else if (4 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(chunk.codec_);
    } else if (5 == field_id && CT_I64 == ctype) {
      ret = reader.read_i64(chunk.num_values_);
    } else if (9 == field_id && CT_I64 == ctype) {
      ret = reader.read_i64(chunk.data_page_offset_);
    } else if (11 == field_id && CT_I64 == ctype) {
      ret = reader.read_i64(chunk.dictionary_page_offset_);
      if (OB_SUCC(ret)) {
        chunk.has_dictionary_page_ = true;
      }
    } else {
      ret = reader.skip(ctype);
    }
  }
  return ret;
}

int parse_column_chunk(ParquetCompactReader &reader, ParquetColumnChunk &chunk)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    int16_t field_id = 0;
    int32_t ctype = 0;
    if (OB_FAIL(reader.read_field_header(last_id, field_id, ctype))) {
    } else if (CT_STOP == ctype) {
      done = true;
    } else if (3 == field_id && CT_STRUCT == ctype) {
      ret = parse_column_meta_data(reader, chunk);
    } else {
      ret = reader.skip(ctype);
    }
  }
  return ret;
}

int parse_row_group(ParquetCompactReader &reader, ParquetFileMeta &meta)
{
  int ret = OB_SUCCESS;
  ParquetRowGroup row_group;
  row_group.chunk_start_ = meta.chunks_.count();
  int16_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    int16_t field_id = 0;
    int32_t ctype = 0;
    if (OB_FAIL(reader.read_field_header(last_id, field_id, ctype))) {
    } else if (CT_STOP == ctype) {
      done = true;
    } else if (1 == field_id && CT_LIST == ctype) {
      int32_t list_size = 0;
      int32_t elem_type = 0;
      if (OB_FAIL(reader.read_list_header(list_size, elem_type))) {
      } else if (CT_STRUCT != elem_type) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        for (int32_t i = 0; OB_SUCC(ret) && i < list_size; ++i) {
          ParquetColumnChunk chunk;
          if (OB_FAIL(parse_column_chunk(reader, chunk))) {
          } else {
            ret = meta.chunks_.push_back(chunk);
          }
        }
      }
    } else if (3 == field_id && CT_I64 == ctype) {
      ret = reader.read_i64(row_group.num_rows_);
    } else {
      ret = reader.skip(ctype);
    }
  }
  if (OB_SUCC(ret)) {
    row_group.chunk_count_ = meta.chunks_.count() - row_group.chunk_start_;
    ret = meta.row_groups_.push_back(row_group);
  }
  return ret;
}

int parse_row_group_list(ParquetCompactReader &reader, ParquetFileMeta &meta)
{
  int ret = OB_SUCCESS;
  int32_t list_size = 0;
  int32_t elem_type = 0;
  if (OB_FAIL(reader.read_list_header(list_size, elem_type))) {
  } else if (CT_STRUCT != elem_type) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int32_t i = 0; OB_SUCC(ret) && i < list_size; ++i) {
      ret = parse_row_group(reader, meta);
    }
  }
  return ret;
}

int parse_file_metadata(ParquetCompactReader &reader, ParquetFileMeta &meta)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    int16_t field_id = 0;
    int32_t ctype = 0;
    if (OB_FAIL(reader.read_field_header(last_id, field_id, ctype))) {
    } else if (CT_STOP == ctype) {
      done = true;
    } else if (1 == field_id && CT_I32 == ctype) {
      int32_t version = 0;
      ret = reader.read_i32(version);
      if (OB_SUCC(ret) && version < 0) {
        ret = OB_ERR_UNEXPECTED;
      }
    } else if (2 == field_id && CT_LIST == ctype) {
      ret = parse_schema_list(reader, meta);
    } else if (3 == field_id && CT_I64 == ctype) {
      ret = reader.read_i64(meta.num_rows_);
    } else if (4 == field_id && CT_LIST == ctype) {
      ret = parse_row_group_list(reader, meta);
    } else {
      ret = reader.skip(ctype);
    }
  }
  return ret;
}

// 从 schema 里挑出叶子列；嵌套 struct 一律不支持
int build_leaf_columns(ObIAllocator &alloc, ParquetFileMeta &meta)
{
  int ret = OB_SUCCESS;
  if (meta.schema_.count() < 2) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 1; OB_SUCC(ret) && i < meta.schema_.count(); ++i) {
    const ParquetSchemaElement &element = meta.schema_.at(i);
    if (element.has_num_children_ && element.num_children_ > 0) {
      // 非叶子节点，即嵌套 group
      ret = OB_NOT_SUPPORTED;
    } else if (!element.has_physical_type_) {
      ret = OB_ERR_UNEXPECTED;
    } else if (element.has_repetition_type_ && RT_REPEATED == element.repetition_type_) {
      ret = OB_NOT_SUPPORTED;
    } else {
      ParquetLeafColumn leaf;
      leaf.physical_type_ = element.physical_type_;
      leaf.nullable_ = (element.has_repetition_type_ && RT_OPTIONAL == element.repetition_type_);
      leaf.max_def_level_ = leaf.nullable_ ? 1 : 0;
      const int64_t name_len = element.name_.length();
      if (name_len < 0) {
        ret = OB_ERR_UNEXPECTED;
      } else if (0 == name_len) {
        leaf.name_.assign_ptr(NULL, 0);
      } else {
        char *name_buf = static_cast<char *>(alloc.alloc(name_len));
        if (OB_ISNULL(name_buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(name_buf, element.name_.ptr(), name_len);
          leaf.name_.assign_ptr(name_buf, static_cast<int32_t>(name_len));
        }
      }
      if (OB_SUCC(ret)) {
        ret = meta.leaves_.push_back(leaf);
      }
    }
  }
  return ret;
}

bool is_same_string(const ObString &left, const ObString &right)
{
  bool same = (left.length() == right.length());
  if (same && left.length() > 0) {
    same = (0 == MEMCMP(left.ptr(), right.ptr(), left.length()));
  }
  return same;
}

// 把每个 chunk 的 path 映射回叶子列下标
int resolve_chunk_columns(ParquetFileMeta &meta)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < meta.chunks_.count(); ++i) {
    ParquetColumnChunk &chunk = meta.chunks_.at(i);
    int64_t column_idx = -1;
    for (int64_t j = 0; j < meta.leaves_.count(); ++j) {
      if (is_same_string(chunk.path_, meta.leaves_.at(j).name_)) {
        column_idx = j;
        break;
      }
    }
    if (column_idx < 0) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      chunk.column_idx_ = static_cast<int32_t>(column_idx);
      if (meta.leaves_.at(column_idx).physical_type_ != chunk.physical_type_) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// 页解析
// ---------------------------------------------------------------------------
int parse_data_page_header(ParquetCompactReader &reader, ParquetDataPageHeader &header)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    int16_t field_id = 0;
    int32_t ctype = 0;
    if (OB_FAIL(reader.read_field_header(last_id, field_id, ctype))) {
    } else if (CT_STOP == ctype) {
      done = true;
    } else if (1 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.num_values_);
    } else if (2 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.encoding_);
    } else if (3 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.definition_level_encoding_);
    } else if (4 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.repetition_level_encoding_);
    } else {
      ret = reader.skip(ctype);
    }
  }
  if (OB_SUCC(ret)) {
    header.valid_ = true;
  }
  return ret;
}

int parse_data_page_header_v2(ParquetCompactReader &reader, ParquetDataPageHeaderV2 &header)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    int16_t field_id = 0;
    int32_t ctype = 0;
    if (OB_FAIL(reader.read_field_header(last_id, field_id, ctype))) {
    } else if (CT_STOP == ctype) {
      done = true;
    } else if (1 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.num_values_);
    } else if (2 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.num_nulls_);
    } else if (3 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.num_rows_);
    } else if (4 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.encoding_);
    } else if (5 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.definition_levels_byte_length_);
    } else if (6 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.repetition_levels_byte_length_);
    } else if (7 == field_id) {
      // compact 协议的 bool 写在字段头里，没有额外字节
      if (CT_BOOLEAN_TRUE == ctype) {
        header.is_compressed_ = true;
      } else if (CT_BOOLEAN_FALSE == ctype) {
        header.is_compressed_ = false;
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
    } else {
      ret = reader.skip(ctype);
    }
  }
  if (OB_SUCC(ret)) {
    header.valid_ = true;
  }
  return ret;
}

int parse_page_header(ParquetCompactReader &reader, ParquetPageHeader &header)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    int16_t field_id = 0;
    int32_t ctype = 0;
    if (OB_FAIL(reader.read_field_header(last_id, field_id, ctype))) {
    } else if (CT_STOP == ctype) {
      done = true;
    } else if (1 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.type_);
    } else if (2 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.uncompressed_page_size_);
    } else if (3 == field_id && CT_I32 == ctype) {
      ret = reader.read_i32(header.compressed_page_size_);
    } else if (5 == field_id && CT_STRUCT == ctype) {
      ret = parse_data_page_header(reader, header.data_page_header_);
    } else if (8 == field_id && CT_STRUCT == ctype) {
      ret = parse_data_page_header_v2(reader, header.data_page_header_v2_);
    } else {
      ret = reader.skip(ctype);
    }
  }
  return ret;
}

// 解出一页里的所有值（含 NULL），追加到 values。
// level_buf 指向 definition level 段（RLE/bit-packed hybrid，不含 4 字节长度前缀），
// value_buf 指向编码后的取值段。两段由调用方按页版本切好。
int append_page_values(const char *level_buf,
                       const int64_t level_len,
                       const char *value_buf,
                       const int64_t value_len,
                       const int32_t num_values,
                       const int32_t max_def_level,
                       const int32_t physical_type,
                       ObIAllocator &alloc,
                       ObArray<ObParquetValue> &values)
{
  int ret = OB_SUCCESS;
  ParquetRleBpDecoder level_decoder;
  ParquetPlainDecoder value_decoder;
  const bool has_def_level = (max_def_level > 0);
  if (num_values < 0 || value_len < 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (has_def_level) {
    if (OB_ISNULL(level_buf)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(level_decoder.init(level_buf, level_len, bit_width_of(max_def_level)))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(value_decoder.init(value_buf, value_len, physical_type))) {
  } else {
    for (int32_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
      ObParquetValue value;
      bool is_null = false;
      if (has_def_level) {
        int32_t level = 0;
        if (OB_FAIL(level_decoder.next_value(level))) {
          if (OB_ITER_END == ret) {
            // level 数据的量少于 num_values，文件损坏
            ret = OB_ERR_UNEXPECTED;
          }
        } else if (level < 0 || level > max_def_level) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          is_null = (level < max_def_level);
        }
      }
      if (OB_SUCC(ret)) {
        if (is_null) {
          value.is_null_ = true;
          value.is_double_ = false;
          value.num_ = 0;
        } else if (OB_FAIL(value_decoder.next_value(alloc, value))) {
        }
      }
      if (OB_SUCC(ret)) {
        ret = values.push_back(value);
      }
    }
  }
  return ret;
}

int read_column_chunk(const char *buf,
                      const int64_t len,
                      const ParquetColumnChunk &chunk,
                      const int32_t max_def_level,
                      ObIAllocator &alloc,
                      ObArray<ObParquetValue> &values)
{
  int ret = OB_SUCCESS;
  if (CODEC_UNCOMPRESSED != chunk.codec_ && CODEC_SNAPPY != chunk.codec_) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("unsupported parquet compression codec", K(chunk.codec_));
  } else if (chunk.num_values_ < 0 || chunk.data_page_offset_ < 0) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int64_t offset = chunk.data_page_offset_;
    int64_t values_read = 0;
    while (OB_SUCC(ret) && values_read < chunk.num_values_) {
      ParquetCompactReader reader;
      ParquetPageHeader header;
      reader.reset(buf, len, offset);
      if (OB_FAIL(parse_page_header(reader, header))) {
      } else if (header.compressed_page_size_ < 0
                 || header.uncompressed_page_size_ < 0
                 || reader.pos() + header.compressed_page_size_ > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const char *page_buf = buf + reader.pos();
        const int64_t page_len = header.compressed_page_size_;
        const int64_t uncompressed_len = header.uncompressed_page_size_;
        if (PAGE_TYPE_DATA == header.type_) {
          // V1：整个页体一起压缩，level 段带 4 字节长度前缀
          const ParquetDataPageHeader &v1 = header.data_page_header_;
          const char *body = page_buf;
          int64_t body_len = page_len;
          const int32_t num_values = v1.num_values_;
          if (!v1.valid_) {
            ret = OB_ERR_UNEXPECTED;
          } else if (ENCODING_PLAIN != v1.encoding_) {
            ret = OB_NOT_SUPPORTED;
          } else if (num_values <= 0) {
            ret = OB_ERR_UNEXPECTED;
          } else if (CODEC_UNCOMPRESSED != chunk.codec_) {
            char *inflated = NULL;
            if (uncompressed_len <= 0) {
              ret = OB_ERR_UNEXPECTED;
            } else if (OB_ISNULL(inflated = static_cast<char *>(alloc.alloc(uncompressed_len)))) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (OB_FAIL(snappy_decompress(page_buf, page_len, inflated, uncompressed_len))) {
            } else {
              body = inflated;
              body_len = uncompressed_len;
            }
          }
          const char *level_buf = NULL;
          int64_t level_len = 0;
          int64_t data_pos = 0;
          if (OB_FAIL(ret)) {
          } else if (max_def_level > 0) {
            if (body_len < 4) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              const uint32_t lvl_len = read_le_u32(body);
              if (static_cast<int64_t>(lvl_len) > body_len - 4) {
                ret = OB_ERR_UNEXPECTED;
              } else {
                level_buf = body + 4;
                level_len = static_cast<int64_t>(lvl_len);
                data_pos = 4 + level_len;
              }
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(append_page_values(level_buf,
                                                level_len,
                                                body + data_pos,
                                                body_len - data_pos,
                                                num_values,
                                                max_def_level,
                                                chunk.physical_type_,
                                                alloc,
                                                values))) {
          } else {
            values_read += num_values;
            offset = reader.pos() + page_len;
          }
        } else if (PAGE_TYPE_DATA_V2 == header.type_) {
          // V2：rep/def 段不压缩，只有取值段压缩，且 level 段不带长度前缀
          const ParquetDataPageHeaderV2 &v2 = header.data_page_header_v2_;
          const int64_t rep_len = v2.repetition_levels_byte_length_;
          const int64_t def_len = v2.definition_levels_byte_length_;
          const int32_t num_values = v2.num_values_;
          if (!v2.valid_) {
            ret = OB_ERR_UNEXPECTED;
          } else if (ENCODING_PLAIN != v2.encoding_) {
            ret = OB_NOT_SUPPORTED;
          } else if (num_values <= 0) {
            ret = OB_ERR_UNEXPECTED;
          } else if (rep_len < 0 || def_len < 0
                     || rep_len + def_len > page_len
                     || rep_len + def_len > uncompressed_len) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const char *level_buf = NULL;
            int64_t level_len = 0;
            if (max_def_level > 0) {
              level_buf = page_buf + rep_len;
              level_len = def_len;
            }
            const char *raw_value_buf = page_buf + rep_len + def_len;
            const int64_t raw_value_len = page_len - rep_len - def_len;
            const int64_t value_uncompressed_len = uncompressed_len - rep_len - def_len;
            const char *value_buf = raw_value_buf;
            int64_t value_len = raw_value_len;
            if (v2.is_compressed_ && CODEC_UNCOMPRESSED != chunk.codec_
                && value_uncompressed_len > 0) {
              char *inflated = NULL;
              if (OB_ISNULL(inflated =
                                static_cast<char *>(alloc.alloc(value_uncompressed_len)))) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              } else if (OB_FAIL(snappy_decompress(raw_value_buf,
                                                   raw_value_len,
                                                   inflated,
                                                   value_uncompressed_len))) {
              } else {
                value_buf = inflated;
                value_len = value_uncompressed_len;
              }
            }
            if (OB_FAIL(ret)) {
            } else if (OB_FAIL(append_page_values(level_buf,
                                                  level_len,
                                                  value_buf,
                                                  value_len,
                                                  num_values,
                                                  max_def_level,
                                                  chunk.physical_type_,
                                                  alloc,
                                                  values))) {
            } else {
              values_read += num_values;
              offset = reader.pos() + page_len;
            }
          }
        } else {
          // DICTIONARY_PAGE / INDEX_PAGE 不支持
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("unsupported parquet page type", K(header.type_));
        }
      }
    }
  }
  return ret;
}

int read_row_groups(const char *buf,
                    const int64_t len,
                    ParquetFileMeta &meta,
                    ObIAllocator &alloc,
                    ObArray<ObParquetColumn> &columns,
                    int64_t &parsed_rows)
{
  int ret = OB_SUCCESS;
  parsed_rows = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < meta.row_groups_.count(); ++i) {
    const ParquetRowGroup &row_group = meta.row_groups_.at(i);
    for (int64_t j = 0; OB_SUCC(ret) && j < row_group.chunk_count_; ++j) {
      const ParquetColumnChunk &chunk = meta.chunks_.at(row_group.chunk_start_ + j);
      if (chunk.column_idx_ < 0 || chunk.column_idx_ >= columns.count()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObParquetColumn &column = columns.at(chunk.column_idx_);
        if (OB_ISNULL(column.values_)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ret = read_column_chunk(buf, len, chunk, column.nullable_ ? 1 : 0, alloc, *column.values_);
        }
      }
    }
    if (OB_SUCC(ret)) {
      // 每个行组结束后，所有列的行数必须都推进到同一个位置
      parsed_rows += row_group.num_rows_;
      for (int64_t k = 0; OB_SUCC(ret) && k < columns.count(); ++k) {
        if (OB_ISNULL(columns.at(k).values_) || columns.at(k).values_->count() != parsed_rows) {
          ret = OB_ERR_UNEXPECTED;
        }
      }
    }
  }
  return ret;
}

template <typename T>
void destroy_and_free(ObIAllocator &alloc, T *&ptr)
{
  if (NULL != ptr) {
    ptr->~T();
    alloc.free(ptr);
    ptr = NULL;
  }
}

ObArray<ObParquetValue> *create_value_array(ObIAllocator &alloc)
{
  ObArray<ObParquetValue> *array = NULL;
  void *mem = alloc.alloc(sizeof(ObArray<ObParquetValue>));
  if (NULL != mem) {
    array = new (mem) ObArray<ObParquetValue>(
        OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator(alloc, "ParquetVals"));
  }
  return array;
}

// 每个叶子列分配一个取值数组
int build_columns(ObIAllocator &alloc,
                  ParquetFileMeta &meta,
                  ObArray<ObParquetColumn> &columns)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < meta.leaves_.count(); ++i) {
    const ParquetLeafColumn &leaf = meta.leaves_.at(i);
    ObParquetColumn column;
    column.name_ = leaf.name_;
    column.nullable_ = leaf.nullable_;
    column.physical_type_ = leaf.physical_type_;
    column.values_ = create_value_array(alloc);
    if (OB_ISNULL(column.values_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ret = columns.push_back(column);
    }
  }
  return ret;
}

// 每列的取值数量必须等于总行数
int check_column_counts(const ObArray<ObParquetColumn> &columns, const int64_t expected_rows)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const ObParquetColumn &column = columns.at(i);
    if (OB_ISNULL(column.values_) || column.values_->count() != expected_rows) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

} // end anonymous namespace

ObParquetFile::ObParquetFile(ObIAllocator &alloc)
  : alloc_(alloc),
    columns_(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator(alloc, "ParquetCols")),
    row_count_(0)
{
}

ObParquetFile::~ObParquetFile()
{
  reset_state();
}

void ObParquetFile::reset_state()
{
  for (int64_t i = 0; i < columns_.count(); ++i) {
    ObParquetColumn &column = columns_.at(i);
    destroy_and_free(alloc_, column.values_);
  }
  columns_.reset();
  row_count_ = 0;
}

int ObParquetFile::parse(const char *buf, const int64_t len)
{
  int ret = OB_SUCCESS;
  reset_state();
  if (OB_ISNULL(buf) || len < PARQUET_MIN_FILE_SIZE) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 != MEMCMP(buf, PARQUET_MAGIC, sizeof(PARQUET_MAGIC))
             || 0 != MEMCMP(buf + len - sizeof(PARQUET_MAGIC), PARQUET_MAGIC, sizeof(PARQUET_MAGIC))) {
    // 头部或尾部没有 PAR1 魔数，不是 parquet 文件
    ret = OB_INVALID_ARGUMENT;
  } else {
    const uint32_t metadata_len = read_le_u32(buf + len - PARQUET_FOOTER_SIZE);
    if (static_cast<int64_t>(metadata_len) <= 0
        || static_cast<int64_t>(metadata_len) > len - PARQUET_MIN_FILE_SIZE) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const int64_t metadata_start = len - PARQUET_FOOTER_SIZE - static_cast<int64_t>(metadata_len);
      ParquetCompactReader reader;
      ParquetFileMeta meta(alloc_);
      int64_t parsed_rows = 0;
      reader.reset(buf + metadata_start, static_cast<int64_t>(metadata_len), 0);
      if (OB_FAIL(parse_file_metadata(reader, meta))) {
      } else if (meta.num_rows_ < 0) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(build_leaf_columns(alloc_, meta))) {
      } else if (OB_FAIL(resolve_chunk_columns(meta))) {
      } else if (OB_FAIL(build_columns(alloc_, meta, columns_))) {
      } else if (OB_FAIL(read_row_groups(buf, len, meta, alloc_, columns_, parsed_rows))) {
      } else if (parsed_rows != meta.num_rows_) {
        // 各 row group 行数之和必须等于 FileMetaData 里的 num_rows
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(check_column_counts(columns_, parsed_rows))) {
      } else {
        row_count_ = parsed_rows;
      }
    }
  }
  return ret;
}

int64_t ObParquetFile::column_count() const
{
  return columns_.count();
}

const ObParquetColumn &ObParquetFile::column(const int64_t idx) const
{
  static ObParquetColumn empty_column;
  const ObParquetColumn *column = &empty_column;
  if (idx >= 0 && idx < columns_.count()) {
    column = &columns_.at(idx);
  }
  return *column;
}

int64_t ObParquetFile::row_count() const
{
  return row_count_;
}

} // end namespace sql
} // end namespace oceanbase
