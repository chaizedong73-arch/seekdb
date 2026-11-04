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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/file_scan/ob_parquet_file_table_reader.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "lib/charset/ob_charset.h"
#include "lib/ob_define.h"
#include "lib/utility/alloc_assist.h"
#include "lib/utility/ob_print_utils.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
int64_t fread_size(::FILE *fp, char *buf, int64_t want)
{
  return static_cast<int64_t>(::fread(buf, 1, static_cast<size_t>(want), fp));
}

// decompress a standard (non framed) snappy block, see
// https://github.com/google/snappy/blob/master/FORMAT.md
int snappy_uncompress(
    const char *src, const int64_t src_len, char *dst, const int64_t dst_cap, int64_t &dst_len)
{
  int ret = OB_SUCCESS;
  dst_len = 0;
  int64_t pos = 0;
  // preamble: varint uncompressed length
  uint64_t ulen = 0;
  int32_t shift = 0;
  while (OB_SUCC(ret)) {
    if (pos >= src_len) {
      ret = OB_INVALID_DATA;
      LOG_WARN("truncated snappy preamble", K(src_len), K(ret));
    } else {
      const uint8_t b = static_cast<uint8_t>(src[pos++]);
      ulen |= static_cast<uint64_t>(b & 0x7F) << shift;
      if (0 == (b & 0x80)) {
        break;
      }
      shift += 7;
      if (shift > 35) {
        ret = OB_INVALID_DATA;
        LOG_WARN("snappy preamble varint too long", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_UNLIKELY(ulen > static_cast<uint64_t>(dst_cap))) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("snappy output does not fit", K(ulen), K(dst_cap), K(ret));
    }
  }
  int64_t dpos = 0;
  while (OB_SUCC(ret) && pos < src_len) {
    const uint8_t tag = static_cast<uint8_t>(src[pos++]);
    const uint8_t elem_type = tag & 3;
    if (0 == elem_type) {
      // literal
      int64_t len = (tag >> 2) + 1;
      if (len > 60) {
        const int64_t nbytes = len - 60;
        len = 0;
        if (pos + nbytes > src_len) {
          ret = OB_INVALID_DATA;
          LOG_WARN("truncated snappy literal length", K(ret));
        } else {
          for (int64_t i = 0; i < nbytes; ++i) {
            len |= static_cast<int64_t>(static_cast<uint8_t>(src[pos + i])) << (8 * i);
          }
          pos += nbytes;
          len += 1;
        }
      }
      if (OB_SUCC(ret)) {
        if (pos + len > src_len || dpos + len > dst_cap) {
          ret = OB_INVALID_DATA;
          LOG_WARN("snappy literal out of range", K(pos), K(len), K(src_len), K(dpos), K(dst_cap),
              K(ret));
        } else {
          MEMCPY(dst + dpos, src + pos, static_cast<size_t>(len));
          pos += len;
          dpos += len;
        }
      }
    } else {
      // copy with offset
      int64_t len = 0;
      int64_t offset = 0;
      if (1 == elem_type) {
        len = ((tag >> 2) & 7) + 4;
        offset = (static_cast<int64_t>(tag >> 5) << 8);
        if (pos >= src_len) {
          ret = OB_INVALID_DATA;
          LOG_WARN("truncated snappy copy1", K(ret));
        } else {
          offset |= static_cast<int64_t>(static_cast<uint8_t>(src[pos++]));
        }
      } else if (2 == elem_type) {
        len = (tag >> 2) + 1;
        if (pos + 2 > src_len) {
          ret = OB_INVALID_DATA;
          LOG_WARN("truncated snappy copy2", K(ret));
        } else {
          offset = static_cast<int64_t>(static_cast<uint8_t>(src[pos]))
              | (static_cast<int64_t>(static_cast<uint8_t>(src[pos + 1])) << 8);
          pos += 2;
        }
      } else {
        len = (tag >> 2) + 1;
        if (pos + 4 > src_len) {
          ret = OB_INVALID_DATA;
          LOG_WARN("truncated snappy copy4", K(ret));
        } else {
          offset = static_cast<int64_t>(static_cast<uint8_t>(src[pos]))
              | (static_cast<int64_t>(static_cast<uint8_t>(src[pos + 1])) << 8)
              | (static_cast<int64_t>(static_cast<uint8_t>(src[pos + 2])) << 16)
              | (static_cast<int64_t>(static_cast<uint8_t>(src[pos + 3])) << 24);
          pos += 4;
        }
      }
      if (OB_SUCC(ret)) {
        if (offset <= 0 || offset > dpos || dpos + len > dst_cap) {
          ret = OB_INVALID_DATA;
          LOG_WARN("snappy copy out of range", K(offset), K(len), K(dpos), K(dst_cap), K(ret));
        } else {
          // the copy may overlap its source, copy byte by byte
          for (int64_t i = 0; i < len; ++i) {
            dst[dpos + i] = dst[dpos - offset + i];
          }
          dpos += len;
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (dpos != static_cast<int64_t>(ulen)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("snappy output length mismatch", K(dpos), K(ulen), K(ret));
    } else {
      dst_len = dpos;
    }
  }
  return ret;
}
} // end anonymous namespace

// ---------------- thrift compact protocol cursor ----------------

int ParquetFileTableReader::ThriftCursor::read_byte(uint8_t &v)
{
  int ret = OB_SUCCESS;
  if (pos_ + 1 > len_) {
    out_of_range_ = true;
    ret = OB_SIZE_OVERFLOW;
  } else {
    v = static_cast<uint8_t>(buf_[pos_++]);
  }
  return ret;
}

int ParquetFileTableReader::ThriftCursor::read_uleb128(uint64_t &v)
{
  int ret = OB_SUCCESS;
  v = 0;
  int32_t shift = 0;
  while (OB_SUCC(ret)) {
    uint8_t b = 0;
    if (OB_FAIL(read_byte(b))) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("thrift uleb128 truncated", K(len_), K(pos_), K(ret));
    } else {
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      if (0 == (b & 0x80)) {
        break;
      }
      shift += 7;
      if (shift > 63) {
        ret = OB_SIZE_OVERFLOW;
        LOG_WARN("thrift uleb128 too long", K(ret));
      }
    }
  }
  return ret;
}

int ParquetFileTableReader::ThriftCursor::read_i16(int16_t &v)
{
  int ret = OB_SUCCESS;
  uint64_t u = 0;
  if (OB_FAIL(read_uleb128(u))) {
    LOG_WARN("failed to read thrift i16", K(ret));
  } else {
    v = static_cast<int16_t>((u >> 1) ^ (~(u & 1) + 1));
  }
  return ret;
}

int ParquetFileTableReader::ThriftCursor::read_i32(int32_t &v)
{
  int ret = OB_SUCCESS;
  uint64_t u = 0;
  if (OB_FAIL(read_uleb128(u))) {
    LOG_WARN("failed to read thrift i32", K(ret));
  } else {
    v = static_cast<int32_t>((u >> 1) ^ (~(u & 1) + 1));
  }
  return ret;
}

int ParquetFileTableReader::ThriftCursor::read_i64(int64_t &v)
{
  int ret = OB_SUCCESS;
  uint64_t u = 0;
  if (OB_FAIL(read_uleb128(u))) {
    LOG_WARN("failed to read thrift i64", K(ret));
  } else {
    v = static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
  }
  return ret;
}

int ParquetFileTableReader::ThriftCursor::read_binary(ObString &s)
{
  int ret = OB_SUCCESS;
  uint64_t len = 0;
  if (OB_FAIL(read_uleb128(len))) {
    LOG_WARN("failed to read thrift binary length", K(ret));
  } else if (pos_ + static_cast<int64_t>(len) > len_) {
    out_of_range_ = true;
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("thrift binary out of range", K(len), K(len_), K(pos_), K(ret));
  } else {
    s.assign_ptr(const_cast<char *>(buf_ + pos_), static_cast<int32_t>(len));
    pos_ += static_cast<int64_t>(len);
  }
  return ret;
}

int ParquetFileTableReader::ThriftCursor::read_list_header(uint8_t &etype, int64_t &size)
{
  int ret = OB_SUCCESS;
  uint8_t b = 0;
  if (OB_FAIL(read_byte(b))) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("thrift list header truncated", K(ret));
  } else {
    size = (b >> 4) & 0x0F;
    etype = b & 0x0F;
    if (15 == size) {
      uint64_t big = 0;
      if (OB_FAIL(read_uleb128(big))) {
        ret = OB_SIZE_OVERFLOW;
        LOG_WARN("thrift list size truncated", K(ret));
      } else {
        size = static_cast<int64_t>(big);
      }
    }
  }
  return ret;
}

int ParquetFileTableReader::ThriftCursor::read_field_header(int16_t &prev_fid, int16_t &fid,
    uint8_t &type)
{
  int ret = OB_SUCCESS;
  uint8_t b = 0;
  if (OB_FAIL(read_byte(b))) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("thrift field header truncated", K(ret));
  } else {
    type = b & 0x0F;
    if (TT_STOP == type) {
      fid = -1;
    } else {
      const uint8_t delta = (b >> 4) & 0x0F;
      if (0 != delta) {
        fid = prev_fid + static_cast<int16_t>(delta);
        prev_fid = fid;
      } else {
        int16_t zid = 0;
        if (OB_FAIL(read_i16(zid))) {
          LOG_WARN("failed to read thrift field id", K(ret));
        } else {
          fid = zid;
          prev_fid = fid;
        }
      }
    }
  }
  return ret;
}

int ParquetFileTableReader::ThriftCursor::skip_value(uint8_t type)
{
  int ret = OB_SUCCESS;
  switch (type) {
    case TT_TRUE:
    case TT_FALSE:
      break;
    case TT_BYTE: {
      if (pos_ + 1 > len_) {
        out_of_range_ = true;
        ret = OB_SIZE_OVERFLOW;
      } else {
        pos_ += 1;
      }
      break;
    }
    case TT_I16:
    case TT_I32:
    case TT_I64: {
      uint64_t dummy = 0;
      if (OB_FAIL(read_uleb128(dummy))) {
        ret = OB_SIZE_OVERFLOW;
      }
      break;
    }
    case TT_DOUBLE: {
      if (pos_ + 8 > len_) {
        out_of_range_ = true;
        ret = OB_SIZE_OVERFLOW;
      } else {
        pos_ += 8;
      }
      break;
    }
    case TT_BINARY: {
      ObString dummy;
      if (OB_FAIL(read_binary(dummy))) {
        ret = OB_SIZE_OVERFLOW;
      }
      break;
    }
    case TT_LIST:
    case TT_SET: {
      uint8_t etype = 0;
      int64_t size = 0;
      if (OB_FAIL(read_list_header(etype, size))) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          if (OB_FAIL(skip_value(etype))) {
            LOG_WARN("failed to skip thrift list element", K(i), K(ret));
          }
        }
      }
      break;
    }
    case TT_MAP: {
      uint8_t b = 0;
      if (OB_FAIL(read_byte(b))) {
        ret = OB_SIZE_OVERFLOW;
      } else if (0 == b) {
        // empty map
      } else {
        if (pos_ + 1 > len_) {
          out_of_range_ = true;
          ret = OB_SIZE_OVERFLOW;
        } else {
          pos_ += 1;  // key/value types
          for (int64_t i = 0; OB_SUCC(ret) && i < b; ++i) {
            if (OB_FAIL(skip_value(TT_BINARY))) {
              LOG_WARN("failed to skip thrift map entry", K(i), K(ret));
            }
          }
        }
      }
      break;
    }
    case TT_STRUCT: {
      int16_t prev = 0;
      int16_t fid = 0;
      uint8_t ftype = 0;
      bool stop = false;
      while (OB_SUCC(ret) && !stop) {
        if (OB_FAIL(read_field_header(prev, fid, ftype))) {
          ret = OB_SIZE_OVERFLOW;
        } else if (-1 == fid) {
          stop = true;
        } else if (OB_FAIL(skip_value(ftype))) {
          LOG_WARN("failed to skip thrift struct field", K(fid), K(ret));
        }
      }
      break;
    }
    default:
      ret = OB_INVALID_DATA;
      LOG_WARN("unknown thrift type to skip", K(type), K(ret));
      break;
  }
  return ret;
}

// ---------------- lifecycle ----------------

ParquetFileTableReader::ParquetFileTableReader(ObIAllocator &alloc)
    : alloc_(alloc),
      cursors_(NULL),
      cursor_count_(0),
      cur_row_group_(0),
      row_group_row_(0),
      rg_opened_(false),
      rows_emitted_(0),
      fp_(NULL),
      hdr_buf_(NULL),
      inited_(false)
{}

ParquetFileTableReader::~ParquetFileTableReader()
{
  close();
}

int ParquetFileTableReader::dup_string_(const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.length() > INT32_MAX)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("string too long", K(src.length()), K(ret));
  } else if (src.empty()) {
    dst = src;
  } else {
    char *buf = static_cast<char *>(alloc_.alloc(src.length()));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to alloc memory for string", K(src.length()), K(ret));
    } else {
      MEMCPY(buf, src.ptr(), static_cast<size_t>(src.length()));
      dst.assign_ptr(buf, static_cast<int32_t>(src.length()));
    }
  }
  return ret;
}

int ParquetFileTableReader::alloc_buf_(char *&buf, int64_t &cap, int64_t need)
{
  int ret = OB_SUCCESS;
  if (need <= cap) {
    // fast path
  } else {
    int64_t new_cap = (cap <= 0 ? need : cap * 2);
    while (new_cap < need) {
      new_cap *= 2;
    }
    char *new_buf = static_cast<char *>(alloc_.alloc(new_cap));
    if (OB_ISNULL(new_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to alloc parquet buffer", K(new_cap), K(ret));
    } else {
      if (OB_NOT_NULL(buf) && cap > 0) {
        alloc_.free(buf);
      }
      buf = new_buf;
      cap = new_cap;
    }
  }
  return ret;
}

int ParquetFileTableReader::free_buf_(char *&buf, int64_t &cap)
{
  if (OB_NOT_NULL(buf)) {
    alloc_.free(buf);
    buf = NULL;
  }
  cap = 0;
  return OB_SUCCESS;
}

int ParquetFileTableReader::init(const ObString &path, const FileReadOptions &options)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(path.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("file table path is empty", K(ret));
  } else if (OB_FAIL(dup_string_(path, path_))) {
    LOG_WARN("failed to copy parquet path", K(ret));
  } else {
    options_.max_rows_ = options.max_rows_;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(hdr_buf_ = static_cast<char *>(alloc_.alloc(HDR_PRE_READ_SIZE)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to alloc parquet header buffer", K(ret));
  } else if (OB_ISNULL(fp_ = ::fopen(path_.ptr(), "rb"))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_USER_ERROR(OB_FILE_NOT_EXIST, path_.length(), path_.ptr());
    LOG_WARN("failed to open parquet file", K(path_), K(ret));
  } else if (OB_FAIL(get_file_stat(file_stat_))) {
    LOG_WARN("failed to get file stat", K(path_), K(ret));
  } else if (OB_UNLIKELY(file_stat_.file_size_ > OB_FILE_TABLE_MAX_FILE_SIZE)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("parquet file is too large", K(path_), K(file_stat_.file_size_), K(ret));
  } else {
    inited_ = true;
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(read_footer_())) {
      LOG_WARN("failed to read parquet footer", K(path_), K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(projected_.reserve(leaf_cols_.count()))) {
      LOG_WARN("failed to reserve projected flags", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < leaf_cols_.count(); ++i) {
        // default: all columns are projected
        if (OB_FAIL(projected_.push_back(true))) {
          LOG_WARN("failed to init projected flags", K(ret));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    cursor_count_ = leaf_cols_.count();
    if (cursor_count_ > 0) {
      if (OB_ISNULL(cursors_ = static_cast<ColCursor *>(alloc_.alloc(
                         sizeof(ColCursor) * static_cast<size_t>(cursor_count_))))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to alloc parquet cursors", K(cursor_count_), K(ret));
      } else {
        for (int64_t i = 0; i < cursor_count_; ++i) {
          new (cursors_ + i) ColCursor();
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
    close();
  }
  return ret;
}

int ParquetFileTableReader::get_column_metas(ObIArray<FileColumnMeta> &columns) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("parquet file table reader is not inited", K(ret));
  } else if (OB_FAIL(columns.assign(columns_))) {
    LOG_WARN("failed to assign column metas", K(ret));
  }
  return ret;
}

int ParquetFileTableReader::set_projection(const ObBitSet<> &col_idxs)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("parquet file table reader is not inited", K(ret));
  } else if (col_idxs.is_empty()) {
    // empty bit set: all columns are needed
    for (int64_t i = 0; OB_SUCC(ret) && i < projected_.count(); ++i) {
      projected_.at(i) = true;
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < projected_.count(); ++i) {
      projected_.at(i) = col_idxs.has_member(i);
    }
  }
  return ret;
}

// ---------------- footer ----------------

int ParquetFileTableReader::read_footer_()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(file_stat_.file_size_ < FOOTER_MAGIC_SIZE)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("parquet file is too small", K(file_stat_.file_size_), K(ret));
  } else {
    // the last 8 bytes: 4 bytes footer length (little endian) + magic "PAR1"
    char tail[FOOTER_MAGIC_SIZE];
    if (0 != ::fseek(fp_, file_stat_.file_size_ - FOOTER_MAGIC_SIZE, SEEK_SET)) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to seek parquet tail", K(path_), K(ret));
    } else if (FOOTER_MAGIC_SIZE != fread_size(fp_, tail, FOOTER_MAGIC_SIZE)) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to read parquet tail", K(path_), K(ret));
    } else if (0 != MEMCMP(tail + 4, "PAR1", 4)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("bad parquet magic", K(path_), K(ret));
    } else {
      const int64_t footer_len = static_cast<int64_t>(static_cast<uint8_t>(tail[0]))
          | (static_cast<int64_t>(static_cast<uint8_t>(tail[1])) << 8)
          | (static_cast<int64_t>(static_cast<uint8_t>(tail[2])) << 16)
          | (static_cast<int64_t>(static_cast<uint8_t>(tail[3])) << 24);
      if (footer_len <= 0 || footer_len > file_stat_.file_size_ - FOOTER_MAGIC_SIZE) {
        ret = OB_INVALID_DATA;
        LOG_WARN("bad parquet footer length", K(path_), K(footer_len), K(file_stat_.file_size_),
            K(ret));
      } else {
        char *footer_buf = static_cast<char *>(alloc_.alloc(footer_len));
        if (OB_ISNULL(footer_buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("failed to alloc parquet footer buffer", K(footer_len), K(ret));
        } else if (0 != ::fseek(fp_, file_stat_.file_size_ - FOOTER_MAGIC_SIZE - footer_len,
                       SEEK_SET)) {
          ret = OB_IO_ERROR;
          LOG_WARN("failed to seek parquet footer", K(path_), K(footer_len), K(ret));
        } else if (footer_len != fread_size(fp_, footer_buf, footer_len)) {
          ret = OB_IO_ERROR;
          LOG_WARN("failed to read parquet footer", K(path_), K(footer_len), K(ret));
        } else {
          ThriftCursor tc;
          tc.reset(footer_buf, footer_len);
          if (OB_FAIL(parse_file_meta_(tc))) {
            LOG_WARN("failed to parse parquet footer", K(path_), K(ret));
          }
        }
        alloc_.free(footer_buf);
      }
    }
  }
  return ret;
}

int ParquetFileTableReader::parse_file_meta_(ThriftCursor &tc)
{
  int ret = OB_SUCCESS;
  int16_t prev = 0;
  int16_t fid = 0;
  uint8_t ftype = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    if (OB_FAIL(tc.read_field_header(prev, fid, ftype))) {
      LOG_WARN("failed to read parquet file meta field", K(ret));
    } else if (-1 == fid) {
      stop = true;
    } else if (1 == fid) {
      int32_t version = 0;
      if (OB_FAIL(tc.read_i32(version))) {
        LOG_WARN("failed to read parquet version", K(ret));
      }
    } else if (2 == fid) {
      // schema: list<SchemaElement>
      uint8_t etype = 0;
      int64_t list_size = 0;
      if (OB_FAIL(tc.read_list_header(etype, list_size))) {
        LOG_WARN("failed to read parquet schema list header", K(ret));
      } else if (OB_UNLIKELY(etype != TT_STRUCT)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet schema element must be a struct", K(etype), K(ret));
      } else if (OB_UNLIKELY(list_size <= 0)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet schema is empty", K(list_size), K(ret));
      } else {
        // the first element is the schema root, its num_children marks the
        // number of leaf columns
        LeafCol root;
        int64_t root_children = 0;
        if (OB_FAIL(parse_schema_element_(tc, root, root_children))) {
          LOG_WARN("failed to parse parquet schema root", K(ret));
        } else if (OB_UNLIKELY(root_children <= 0)) {
          ret = OB_INVALID_DATA;
          LOG_WARN("parquet schema root has no children", K(root_children), K(ret));
        } else if (OB_UNLIKELY(root_children > OB_FILE_TABLE_MAX_COLUMN_COUNT)) {
          ret = OB_SIZE_OVERFLOW;
          LOG_WARN("parquet schema has too many columns", K(root_children), K(ret));
        } else {
          int64_t remaining = root_children;
          for (int64_t k = 1; OB_SUCC(ret) && k < list_size; ++k) {
            LeafCol leaf;
            int64_t elem_children = 0;
            if (OB_FAIL(parse_schema_element_(tc, leaf, elem_children))) {
              LOG_WARN("failed to parse parquet schema element", K(k), K(ret));
            } else if (elem_children != 0) {
              ret = OB_NOT_SUPPORTED;
              LOG_WARN("nested parquet schema is not supported", K(leaf.name_), K(ret));
            } else if (remaining > 0) {
              --remaining;
              if (OB_UNLIKELY(leaf.name_.empty())) {
                ret = OB_INVALID_DATA;
                LOG_WARN("parquet leaf column has no name", K(k), K(ret));
              } else if (OB_UNLIKELY(PQT_INT96 == leaf.pq_type_)) {
                ret = OB_NOT_SUPPORTED;
                LOG_WARN("parquet INT96 column is not supported", K(leaf.name_), K(ret));
              } else if (OB_UNLIKELY(FRT_REPEATED == leaf.repetition_)) {
                ret = OB_NOT_SUPPORTED;
                LOG_WARN("parquet repeated column is not supported", K(leaf.name_), K(ret));
              } else {
                ObObjMeta type;
                if (OB_FAIL(map_leaf_type_(leaf, type))) {
                  LOG_WARN("failed to map parquet column type", K(leaf.name_), K(ret));
                } else if (OB_FAIL(leaf_cols_.push_back(leaf))) {
                  LOG_WARN("failed to push parquet leaf column", K(ret));
                } else {
                  FileColumnMeta col;
                  col.name_ = leaf.name_;
                  col.type_ = type;
                  if (OB_FAIL(columns_.push_back(col))) {
                    LOG_WARN("failed to push parquet column meta", K(ret));
                  }
                }
              }
            }
            // elements beyond the declared root children are ignored (they
            // were already parsed and discarded above)
          }
          if (OB_SUCC(ret) && remaining > 0) {
            ret = OB_INVALID_DATA;
            LOG_WARN("parquet schema list is shorter than the root children", K(remaining),
                K(list_size), K(ret));
          }
        }
      }
    } else if (3 == fid) {
      int64_t num_rows = 0;
      if (OB_FAIL(tc.read_i64(num_rows))) {
        LOG_WARN("failed to read parquet num rows", K(ret));
      }
    } else if (4 == fid) {
      // row_groups: list<RowGroup>
      uint8_t etype = 0;
      int64_t list_size = 0;
      if (OB_FAIL(tc.read_list_header(etype, list_size))) {
        LOG_WARN("failed to read parquet row group list header", K(ret));
      } else if (OB_UNLIKELY(etype != TT_STRUCT)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet row group must be a struct", K(etype), K(ret));
      } else {
        for (int64_t k = 0; OB_SUCC(ret) && k < list_size; ++k) {
          if (OB_FAIL(parse_row_group_(tc))) {
            LOG_WARN("failed to parse parquet row group", K(k), K(ret));
          }
        }
      }
    } else {
      if (OB_FAIL(tc.skip_value(ftype))) {
        LOG_WARN("failed to skip parquet file meta field", K(fid), K(ret));
      }
    }
  }
  return ret;
}

int ParquetFileTableReader::parse_schema_element_(ThriftCursor &tc, LeafCol &elem,
    int64_t &num_children)
{
  int ret = OB_SUCCESS;
  int16_t prev = 0;
  int16_t fid = 0;
  uint8_t ftype = 0;
  bool stop = false;
  num_children = 0;
  while (OB_SUCC(ret) && !stop) {
    if (OB_FAIL(tc.read_field_header(prev, fid, ftype))) {
      LOG_WARN("failed to read parquet schema element field", K(ret));
    } else if (-1 == fid) {
      stop = true;
    } else if (1 == fid) {
      int32_t v = 0;
      if (OB_FAIL(tc.read_i32(v))) {
        LOG_WARN("failed to read parquet schema type", K(ret));
      } else {
        elem.pq_type_ = v;
      }
    } else if (2 == fid) {
      int32_t v = 0;
      if (OB_FAIL(tc.read_i32(v))) {
        LOG_WARN("failed to read parquet type length", K(ret));
      } else {
        elem.type_length_ = v;
      }
    } else if (3 == fid) {
      int32_t v = 0;
      if (OB_FAIL(tc.read_i32(v))) {
        LOG_WARN("failed to read parquet repetition type", K(ret));
      } else {
        elem.repetition_ = v;
      }
    } else if (4 == fid) {
      if (OB_FAIL(tc.read_binary(elem.name_))) {
        LOG_WARN("failed to read parquet column name", K(ret));
      }
    } else if (5 == fid) {
      int32_t v = 0;
      if (OB_FAIL(tc.read_i32(v))) {
        LOG_WARN("failed to read parquet num children", K(ret));
      } else {
        num_children = v;
      }
    } else if (6 == fid) {
      int32_t v = 0;
      if (OB_FAIL(tc.read_i32(v))) {
        LOG_WARN("failed to read parquet converted type", K(ret));
      } else {
        elem.converted_type_ = v;
      }
    } else {
      if (OB_FAIL(tc.skip_value(ftype))) {
        LOG_WARN("failed to skip parquet schema element field", K(fid), K(ret));
      }
    }
  }
  return ret;
}

int ParquetFileTableReader::parse_row_group_(ThriftCursor &tc)
{
  int ret = OB_SUCCESS;
  int16_t prev = 0;
  int16_t fid = 0;
  uint8_t ftype = 0;
  bool stop = false;
  RowGroupMeta rg;
  rg.first_chunk_idx_ = chunks_.count();
  int64_t chunk_count = 0;
  while (OB_SUCC(ret) && !stop) {
    if (OB_FAIL(tc.read_field_header(prev, fid, ftype))) {
      LOG_WARN("failed to read parquet row group field", K(ret));
    } else if (-1 == fid) {
      stop = true;
    } else if (1 == fid) {
      // columns: list<ColumnChunk>
      uint8_t etype = 0;
      int64_t list_size = 0;
      if (OB_FAIL(tc.read_list_header(etype, list_size))) {
        LOG_WARN("failed to read parquet column chunk list header", K(ret));
      } else if (OB_UNLIKELY(etype != TT_STRUCT)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet column chunk must be a struct", K(etype), K(ret));
      } else {
        for (int64_t k = 0; OB_SUCC(ret) && k < list_size; ++k) {
          ChunkMeta chunk;
          if (OB_FAIL(parse_column_chunk_(tc, chunk, chunk_count))) {
            LOG_WARN("failed to parse parquet column chunk", K(k), K(ret));
          } else if (OB_FAIL(chunks_.push_back(chunk))) {
            LOG_WARN("failed to push parquet chunk meta", K(ret));
          } else {
            ++chunk_count;
          }
        }
      }
    } else if (3 == fid) {
      int64_t v = 0;
      if (OB_FAIL(tc.read_i64(v))) {
        LOG_WARN("failed to read parquet row group num rows", K(ret));
      } else {
        rg.num_rows_ = v;
      }
    } else {
      if (OB_FAIL(tc.skip_value(ftype))) {
        LOG_WARN("failed to skip parquet row group field", K(fid), K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(row_groups_.push_back(rg))) {
      LOG_WARN("failed to push parquet row group meta", K(ret));
    }
  }
  return ret;
}

int ParquetFileTableReader::parse_column_chunk_(ThriftCursor &tc, ChunkMeta &chunk,
    int64_t leaf_idx)
{
  int ret = OB_SUCCESS;
  int16_t prev = 0;
  int16_t fid = 0;
  uint8_t ftype = 0;
  bool stop = false;
  int64_t data_page_offset = -1;
  int64_t dict_page_offset = -1;
  while (OB_SUCC(ret) && !stop) {
    if (OB_FAIL(tc.read_field_header(prev, fid, ftype))) {
      LOG_WARN("failed to read parquet column chunk field", K(ret));
    } else if (-1 == fid) {
      stop = true;
    } else if (3 == fid) {
      // meta_data: ColumnMetaData struct
      int16_t mprev = 0;
      int16_t mfid = 0;
      uint8_t mtype = 0;
      bool mstop = false;
      while (OB_SUCC(ret) && !mstop) {
        if (OB_FAIL(tc.read_field_header(mprev, mfid, mtype))) {
          LOG_WARN("failed to read parquet column meta field", K(ret));
        } else if (-1 == mfid) {
          mstop = true;
        } else if (1 == mfid) {
          int32_t v = 0;
          if (OB_FAIL(tc.read_i32(v))) {
            LOG_WARN("failed to read parquet chunk type", K(ret));
          }
        } else if (4 == mfid) {
          int32_t v = 0;
          if (OB_FAIL(tc.read_i32(v))) {
            LOG_WARN("failed to read parquet chunk codec", K(ret));
          } else {
            chunk.codec_ = v;
          }
        } else if (5 == mfid) {
          int64_t v = 0;
          if (OB_FAIL(tc.read_i64(v))) {
            LOG_WARN("failed to read parquet chunk num values", K(ret));
          } else {
            chunk.num_values_ = v;
          }
        } else if (9 == mfid) {
          int64_t v = 0;
          if (OB_FAIL(tc.read_i64(v))) {
            LOG_WARN("failed to read parquet data page offset", K(ret));
          } else {
            data_page_offset = v;
          }
        } else if (11 == mfid) {
          int64_t v = 0;
          if (OB_FAIL(tc.read_i64(v))) {
            LOG_WARN("failed to read parquet dictionary page offset", K(ret));
          } else {
            dict_page_offset = v;
          }
        } else {
          if (OB_FAIL(tc.skip_value(mtype))) {
            LOG_WARN("failed to skip parquet column meta field", K(mfid), K(ret));
          }
        }
      }
    } else {
      if (OB_FAIL(tc.skip_value(ftype))) {
        LOG_WARN("failed to skip parquet column chunk field", K(fid), K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (data_page_offset < 0) {
      ret = OB_INVALID_DATA;
      LOG_WARN("parquet chunk has no data page offset", K(ret));
    } else {
      chunk.data_start_offset_ =
          (dict_page_offset >= 0 && dict_page_offset < data_page_offset) ? dict_page_offset
                                                                         : data_page_offset;
      // flat schema: an optional column has a single definition level
      chunk.max_def_level_ =
          (leaf_idx >= 0 && leaf_idx < leaf_cols_.count()
                  && FRT_OPTIONAL == leaf_cols_.at(leaf_idx).repetition_)
          ? 1
          : 0;
      if (chunk.num_values_ < 0) {
        ret = OB_INVALID_DATA;
        LOG_WARN("bad parquet chunk num values", K(chunk.num_values_), K(ret));
      }
    }
  }
  return ret;
}

int ParquetFileTableReader::map_leaf_type_(const LeafCol &leaf, ObObjMeta &type) const
{
  int ret = OB_SUCCESS;
  switch (leaf.pq_type_) {
    case PQT_BOOLEAN:
      type.set_tinyint();
      break;
    case PQT_INT32:
      if (PCT_DATE == leaf.converted_type_) {
        type.set_date();
      } else {
        type.set_int();
      }
      break;
    case PQT_INT64:
      if (PCT_TIMESTAMP_MILLIS == leaf.converted_type_
          || PCT_TIMESTAMP_MICROS == leaf.converted_type_) {
        type.set_datetime();
      } else {
        type.set_int();
      }
      break;
    case PQT_FLOAT:
    case PQT_DOUBLE:
      type.set_double();
      break;
    case PQT_BYTE_ARRAY:
    case PQT_FIXED_LEN_BYTE_ARRAY:
      type.set_varchar();
      type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      break;
    default:
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported parquet column type", K(leaf.pq_type_), K(leaf.name_), K(ret));
      break;
  }
  return ret;
}

// ---------------- row group iteration ----------------

void ParquetFileTableReader::reset_cursors_()
{
  for (int64_t i = 0; i < cursor_count_; ++i) {
    ColCursor &cur = cursors_[i];
    cur.active_ = false;
    cur.next_page_offset_ = -1;
    cur.values_left_ = 0;
    cur.page_values_left_ = 0;
    cur.page_len_ = 0;
    cur.lv_ptr_ = NULL;
    cur.lv_len_ = 0;
    cur.cur_encoding_ = PE_PLAIN;
    cur.val_pos_ = 0;
    cur.bool_bit_pos_ = 0;
    cur.lrle_pos_ = 0;
    cur.lrle_bit_acc_ = 0;
    cur.lrle_bits_ = 0;
    cur.lrle_run_left_ = 0;
    cur.lrle_literal_ = false;
    cur.lrle_repeat_val_ = 0;
    cur.def_bit_width_ = 0;
    cur.def_consumed_ = 0;
    cur.dict_ready_ = false;
    cur.dict_count_ = 0;
    cur.dict_len_ = 0;
    cur.dict_bit_width_ = 0;
    cur.irle_pos_ = 0;
    cur.irle_bit_acc_ = 0;
    cur.irle_bits_ = 0;
    cur.irle_run_left_ = 0;
    cur.irle_literal_ = false;
    cur.irle_repeat_val_ = 0;
    cur.irle_started_ = false;
  }
}

int ParquetFileTableReader::open_row_group_(int64_t rg_idx)
{
  int ret = OB_SUCCESS;
  reset_cursors_();
  const RowGroupMeta &rg = row_groups_.at(rg_idx);
  for (int64_t col = 0; OB_SUCC(ret) && col < leaf_cols_.count(); ++col) {
    ColCursor &cur = cursors_[col];
    if (!projected_.at(col)) {
      cur.active_ = false;
    } else {
      const int64_t chunk_idx = rg.first_chunk_idx_ + col;
      if (OB_UNLIKELY(chunk_idx < 0 || chunk_idx >= chunks_.count())) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet chunk index out of range", K(chunk_idx), K(chunks_.count()), K(ret));
      } else {
        const ChunkMeta &chunk = chunks_.at(chunk_idx);
        cur.active_ = true;
        cur.next_page_offset_ = chunk.data_start_offset_;
        cur.values_left_ = chunk.num_values_;
        cur.page_values_left_ = 0;
        cur.def_bit_width_ = 0;
        cur.def_consumed_ = 0;
        cur.dict_ready_ = false;
        cur.dict_count_ = 0;
        cur.irle_started_ = false;
      }
    }
  }
  return ret;
}

int ParquetFileTableReader::decode_dictionary_page_(ColCursor &cur, int64_t col_idx,
    const PageHeaderInfo &info)
{
  int ret = OB_SUCCESS;
  const LeafCol &leaf = leaf_cols_.at(col_idx);
  if (OB_UNLIKELY(info.dict_num_values_ < 0)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("bad parquet dictionary num values", K(info.dict_num_values_), K(ret));
  } else {
    if (OB_NOT_NULL(cur.dict_objs_) && cur.dict_count_ < info.dict_num_values_) {
      alloc_.free(cur.dict_objs_);
      cur.dict_objs_ = NULL;
      cur.dict_count_ = 0;
    }
    if (OB_NOT_NULL(cur.dict_objs_)) {
      // re-construct the plain objects before decoding into them
      for (int64_t i = 0; i < cur.dict_count_; ++i) {
        cur.dict_objs_[i].~ObObj();
      }
    }
    if (info.dict_num_values_ > 0 && OB_ISNULL(cur.dict_objs_)) {
      cur.dict_objs_ = static_cast<ObObj *>(
          alloc_.alloc(sizeof(ObObj) * static_cast<size_t>(info.dict_num_values_)));
      if (OB_ISNULL(cur.dict_objs_)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to alloc parquet dictionary", K(info.dict_num_values_), K(ret));
      } else {
        cur.dict_count_ = info.dict_num_values_;
      }
    }
    if (OB_SUCC(ret)) {
      cur.dict_count_ = info.dict_num_values_;
      int64_t pos = 0;
      int64_t bit_pos = 0;
      int64_t built = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < cur.dict_count_; ++i) {
        new (cur.dict_objs_ + i) ObObj();
        ++built;
        if (OB_FAIL(decode_plain_from_(cur.dict_buf_, cur.dict_len_, pos, bit_pos, leaf,
                cur.dict_objs_[i]))) {
          LOG_WARN("failed to decode parquet dictionary value", K(i), K(ret));
        }
      }
      if (OB_FAIL(ret)) {
        // keep dict_count_ in sync with the number of constructed objects so
        // that the destructor loop in close() never touches raw memory
        cur.dict_count_ = built;
      }
    }
    if (OB_SUCC(ret)) {
      cur.dict_ready_ = true;
    }
  }
  return ret;
}

int ParquetFileTableReader::load_next_page_(ColCursor &cur, int64_t col_idx, bool &has_page)
{
  int ret = OB_SUCCESS;
  has_page = false;
  const ChunkMeta &chunk = chunks_.at(row_groups_.at(cur_row_group_).first_chunk_idx_ + col_idx);
  const LeafCol &leaf = leaf_cols_.at(col_idx);
  const int64_t base = cur.next_page_offset_;
  if (base < 0 || base >= file_stat_.file_size_) {
    // no more pages
  } else {
    PageHeaderInfo info;
    int64_t hdr_consumed = 0;
    int64_t want = HDR_PRE_READ_SIZE;
    for (bool hdr_ok = false; OB_SUCC(ret) && !hdr_ok;) {
      if (OB_UNLIKELY(0 != ::fseek(fp_, base, SEEK_SET))) {
        ret = OB_IO_ERROR;
        LOG_WARN("failed to seek parquet page header", K(base), K(ret));
      } else {
        const int64_t got = fread_size(fp_, hdr_buf_, want);
        if (got <= 0) {
          if (0 != ::ferror(fp_)) {
            ret = OB_IO_ERROR;
            LOG_WARN("failed to read parquet page header", K(base), K(ret));
          } else {
            // eof: no more pages
            break;
          }
        } else {
          ThriftCursor tc;
          tc.reset(hdr_buf_, got);
          const int parse_ret = parse_page_header_(tc, info);
          if (OB_SIZE_OVERFLOW == parse_ret && tc.out_of_range_) {
            // the header is larger than the pre-read buffer: retry with a
            // bigger pre-read
            ret = OB_SUCCESS;
            want *= 2;
            if (want > HDR_MAX_READ_SIZE) {
              ret = OB_INVALID_DATA;
              LOG_WARN("parquet page header is too large", K(want), K(ret));
            }
          } else if (OB_FAIL(parse_ret)) {
            ret = parse_ret;
            LOG_WARN("failed to parse parquet page header", K(base), K(ret));
          } else {
            hdr_ok = true;
            hdr_consumed = tc.pos_;
          }
        }
      }
    }
    if (OB_SUCC(ret) && hdr_ok) {
      const int64_t body_offset = base + hdr_consumed;
      cur.next_page_offset_ = body_offset + info.compressed_size_;
      if (OB_UNLIKELY(info.compressed_size_ < 0 || info.uncompressed_size_ < 0)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("bad parquet page sizes", K(info.compressed_size_), K(info.uncompressed_size_),
            K(ret));
      } else if (PP_INDEX_PAGE == info.page_type_) {
        // skip index pages and let the caller retry
        has_page = false;
      } else if (OB_UNLIKELY(PP_DATA_PAGE != info.page_type_
                             && PP_DICTIONARY_PAGE != info.page_type_
                             && PP_DATA_PAGE_V2 != info.page_type_)) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unsupported parquet page type", K(info.page_type_), K(ret));
      } else if (OB_UNLIKELY(PC_UNCOMPRESSED != chunk.codec_ && PC_SNAPPY != chunk.codec_)) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unsupported parquet compression codec", K(chunk.codec_), K(ret));
      } else {
        // read the page body
        if (OB_FAIL(alloc_buf_(cur.page_comp_buf_, cur.page_comp_cap_, info.compressed_size_))) {
          LOG_WARN("failed to alloc parquet page buffer", K(info.compressed_size_), K(ret));
        } else if (0 != ::fseek(fp_, body_offset, SEEK_SET)) {
          ret = OB_IO_ERROR;
          LOG_WARN("failed to seek parquet page body", K(body_offset), K(ret));
        } else if (info.compressed_size_ != fread_size(fp_, cur.page_comp_buf_,
                           info.compressed_size_)) {
          ret = OB_IO_ERROR;
          LOG_WARN("failed to read parquet page body", K(info.compressed_size_), K(ret));
        } else if (PP_DICTIONARY_PAGE == info.page_type_) {
          // dictionary page body: plain encoded values, decompressed into the
          // dedicated dictionary buffer (data pages reuse page_buf_)
          if (PC_SNAPPY == chunk.codec_) {
            if (OB_FAIL(alloc_buf_(cur.dict_buf_, cur.dict_cap_, info.uncompressed_size_))) {
              LOG_WARN("failed to alloc parquet dict buffer", K(info.uncompressed_size_), K(ret));
            } else {
              int64_t out_len = 0;
              if (OB_FAIL(snappy_uncompress(cur.page_comp_buf_, info.compressed_size_,
                      cur.dict_buf_, cur.dict_cap_, out_len))) {
                LOG_WARN("failed to uncompress parquet dictionary page", K(ret));
              } else {
                cur.dict_len_ = out_len;
              }
            }
          } else {
            if (OB_FAIL(alloc_buf_(cur.dict_buf_, cur.dict_cap_, info.compressed_size_))) {
              LOG_WARN("failed to alloc parquet dict buffer", K(info.compressed_size_), K(ret));
            } else {
              MEMCPY(cur.dict_buf_, cur.page_comp_buf_, static_cast<size_t>(info.compressed_size_));
              cur.dict_len_ = info.compressed_size_;
            }
          }
          if (OB_SUCC(ret)) {
            if (OB_FAIL(decode_dictionary_page_(cur, col_idx, info))) {
              LOG_WARN("failed to decode parquet dictionary page", K(ret));
            } else {
              // continue with the next page
              has_page = false;
            }
          }
        } else {
          // data page (v1 or v2)
          const int64_t num_values =
              (PP_DATA_PAGE_V2 == info.page_type_) ? info.v2_num_values_ : info.v1_num_values_;
          const int32_t encoding =
              (PP_DATA_PAGE_V2 == info.page_type_) ? info.v2_encoding_ : info.v1_encoding_;
          if (OB_UNLIKELY(num_values < 0)) {
            ret = OB_INVALID_DATA;
            LOG_WARN("bad parquet data page num values", K(num_values), K(ret));
          } else if (OB_UNLIKELY(PE_PLAIN != encoding && PE_PLAIN_DICTIONARY != encoding
                                 && PE_RLE_DICTIONARY != encoding)) {
            ret = OB_NOT_SUPPORTED;
            LOG_WARN("unsupported parquet value encoding", K(encoding), K(ret));
          } else {
            int64_t values_len = 0;
            if (PP_DATA_PAGE_V2 == info.page_type_) {
              // v2: [rep levels][def levels (uncompressed)][values (compressed)]
              const int64_t levels_len =
                  static_cast<int64_t>(info.v2_def_len_) + info.v2_rep_len_;
              if (OB_UNLIKELY(levels_len > info.compressed_size_)) {
                ret = OB_INVALID_DATA;
                LOG_WARN("bad parquet v2 levels length", K(levels_len), K(info.compressed_size_),
                    K(ret));
              } else {
                values_len = info.compressed_size_ - levels_len;
                cur.lv_ptr_ = cur.page_comp_buf_ + info.v2_rep_len_;
                cur.lv_len_ = info.v2_def_len_;
                if (info.v2_is_compressed_) {
                  if (PC_SNAPPY != chunk.codec_) {
                    ret = OB_NOT_SUPPORTED;
                    LOG_WARN("unsupported parquet v2 compression codec", K(chunk.codec_), K(ret));
                  } else if (OB_FAIL(alloc_buf_(cur.page_buf_, cur.page_cap_, values_len))) {
                    LOG_WARN("failed to alloc parquet v2 value buffer", K(values_len), K(ret));
                  } else {
                    int64_t out_len = 0;
                    if (OB_FAIL(snappy_uncompress(cur.page_comp_buf_ + levels_len, values_len,
                            cur.page_buf_, cur.page_cap_, out_len))) {
                      LOG_WARN("failed to uncompress parquet v2 values", K(ret));
                    } else {
                      cur.page_len_ = out_len;
                    }
                  }
                } else {
                  if (OB_FAIL(alloc_buf_(cur.page_buf_, cur.page_cap_, values_len))) {
                    LOG_WARN("failed to alloc parquet v2 value buffer", K(values_len), K(ret));
                  } else {
                    MEMCPY(cur.page_buf_, cur.page_comp_buf_ + levels_len,
                        static_cast<size_t>(values_len));
                    cur.page_len_ = values_len;
                  }
                }
              }
            } else {
              // v1: [rep levels][def levels][values] compressed together
              if (PC_SNAPPY == chunk.codec_) {
                if (OB_FAIL(alloc_buf_(cur.page_buf_, cur.page_cap_, info.uncompressed_size_))) {
                  LOG_WARN("failed to alloc parquet v1 value buffer", K(info.uncompressed_size_),
                      K(ret));
                } else {
                  int64_t out_len = 0;
                  if (OB_FAIL(snappy_uncompress(cur.page_comp_buf_, info.compressed_size_,
                          cur.page_buf_, cur.page_cap_, out_len))) {
                    LOG_WARN("failed to uncompress parquet v1 page", K(ret));
                  } else if (OB_UNLIKELY(out_len != info.uncompressed_size_)) {
                    ret = OB_INVALID_DATA;
                    LOG_WARN("parquet v1 page size mismatch", K(out_len),
                        K(info.uncompressed_size_), K(ret));
                  } else {
                    cur.page_len_ = out_len;
                  }
                }
              } else {
                if (OB_FAIL(alloc_buf_(cur.page_buf_, cur.page_cap_, info.compressed_size_))) {
                  LOG_WARN("failed to alloc parquet v1 value buffer", K(info.compressed_size_),
                      K(ret));
                } else {
                  MEMCPY(cur.page_buf_, cur.page_comp_buf_,
                      static_cast<size_t>(info.compressed_size_));
                  cur.page_len_ = info.compressed_size_;
                }
              }
              if (OB_SUCC(ret)) {
                // in a v1 page the levels are part of the decompressed body
                cur.lv_ptr_ = cur.page_buf_;
                cur.lv_len_ = cur.page_len_;
              }
            }
            if (OB_SUCC(ret)) {
              // materialize the definition levels (flat schema: max_def_level_
              // is at most 1) and locate the value section
              cur.cur_encoding_ = encoding;
              cur.page_values_left_ = num_values;
              cur.bool_bit_pos_ = 0;
              cur.lrle_pos_ = 0;
              cur.lrle_bit_acc_ = 0;
              cur.lrle_bits_ = 0;
              cur.lrle_run_left_ = 0;
              cur.lrle_literal_ = false;
              cur.lrle_repeat_val_ = 0;
              cur.def_consumed_ = 0;
              cur.def_bit_width_ = 0;
              cur.irle_started_ = false;
              if (chunk.max_def_level_ > 0) {
                if (OB_FAIL(alloc_buf_(reinterpret_cast<char *&>(cur.def_levels_),
                        cur.def_levels_cap_, num_values))) {
                  LOG_WARN("failed to alloc parquet def levels", K(num_values), K(ret));
                } else {
                  cur.def_bit_width_ = 1;  // flat schema: def level 0 or 1
                  for (int64_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
                    uint32_t def = 0;
                    if (OB_FAIL(rle_next_(cur.lv_ptr_, cur.lv_len_, cur.lrle_pos_,
                            cur.lrle_bit_acc_, cur.lrle_bits_, cur.lrle_run_left_,
                            cur.lrle_literal_, cur.lrle_repeat_val_, cur.def_bit_width_, def))) {
                      LOG_WARN("failed to materialize parquet def levels", K(col_idx), K(i), K(ret));
                    } else if (OB_UNLIKELY(def > static_cast<uint32_t>(chunk.max_def_level_))) {
                      ret = OB_INVALID_DATA;
                      LOG_WARN("parquet def level out of range", K(def),
                          K(chunk.max_def_level_), K(ret));
                    } else {
                      cur.def_levels_[i] = static_cast<uint8_t>(def);
                    }
                  }
                  if (OB_SUCC(ret)) {
                    // v1: the values start after the level stream inside
                    // page_buf_; v2: the values were relocated to page_buf_
                    cur.val_pos_ = (PP_DATA_PAGE_V2 == info.page_type_) ? 0 : cur.lrle_pos_;
                  }
                }
              } else {
                cur.val_pos_ = 0;
              }
              if (OB_SUCC(ret)) {
                has_page = true;
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ParquetFileTableReader::parse_page_header_(ThriftCursor &tc, PageHeaderInfo &info)
{
  int ret = OB_SUCCESS;
  int16_t prev = 0;
  int16_t fid = 0;
  uint8_t ftype = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    if (OB_FAIL(tc.read_field_header(prev, fid, ftype))) {
      if (!tc.out_of_range_) {
        LOG_WARN("failed to read parquet page header field", K(ret));
      }
    } else if (-1 == fid) {
      stop = true;
    } else if (1 == fid) {
      int32_t v = 0;
      if (OB_FAIL(tc.read_i32(v))) {
        LOG_WARN("failed to read parquet page type", K(ret));
      } else {
        info.page_type_ = v;
      }
    } else if (2 == fid) {
      int32_t v = 0;
      if (OB_FAIL(tc.read_i32(v))) {
        LOG_WARN("failed to read parquet uncompressed page size", K(ret));
      } else {
        info.uncompressed_size_ = v;
      }
    } else if (3 == fid) {
      int32_t v = 0;
      if (OB_FAIL(tc.read_i32(v))) {
        LOG_WARN("failed to read parquet compressed page size", K(ret));
      } else {
        info.compressed_size_ = v;
      }
    } else if (5 == fid) {
      info.has_v1_ = true;
      // DataPageHeader: 1 num_values, 2 encoding
      int16_t dprev = 0;
      int16_t dfid = 0;
      uint8_t dtype = 0;
      bool dstop = false;
      while (OB_SUCC(ret) && !dstop) {
        if (OB_FAIL(tc.read_field_header(dprev, dfid, dtype))) {
          if (!tc.out_of_range_) {
            LOG_WARN("failed to read parquet data page header field", K(ret));
          }
        } else if (-1 == dfid) {
          dstop = true;
        } else if (1 == dfid) {
          int32_t v = 0;
          if (OB_FAIL(tc.read_i32(v))) {
            LOG_WARN("failed to read parquet page num values", K(ret));
          } else {
            info.v1_num_values_ = v;
          }
        } else if (2 == dfid) {
          int32_t v = 0;
          if (OB_FAIL(tc.read_i32(v))) {
            LOG_WARN("failed to read parquet page encoding", K(ret));
          } else {
            info.v1_encoding_ = v;
          }
        } else {
          if (OB_FAIL(tc.skip_value(dtype))) {
            LOG_WARN("failed to skip parquet data page header field", K(dfid), K(ret));
          }
        }
      }
    } else if (7 == fid) {
      info.has_dict_ = true;
      // DictionaryPageHeader: 1 num_values, 2 encoding
      int16_t dprev = 0;
      int16_t dfid = 0;
      uint8_t dtype = 0;
      bool dstop = false;
      while (OB_SUCC(ret) && !dstop) {
        if (OB_FAIL(tc.read_field_header(dprev, dfid, dtype))) {
          if (!tc.out_of_range_) {
            LOG_WARN("failed to read parquet dict page header field", K(ret));
          }
        } else if (-1 == dfid) {
          dstop = true;
        } else if (1 == dfid) {
          int32_t v = 0;
          if (OB_FAIL(tc.read_i32(v))) {
            LOG_WARN("failed to read parquet dict num values", K(ret));
          } else {
            info.dict_num_values_ = v;
          }
        } else if (2 == dfid) {
          int32_t v = 0;
          if (OB_FAIL(tc.read_i32(v))) {
            LOG_WARN("failed to read parquet dict encoding", K(ret));
          } else {
            info.dict_encoding_ = v;
          }
        } else {
          if (OB_FAIL(tc.skip_value(dtype))) {
            LOG_WARN("failed to skip parquet dict page header field", K(dfid), K(ret));
          }
        }
      }
    } else if (8 == fid) {
      info.has_v2_ = true;
      // DataPageHeaderV2: 1 num_values, 4 encoding, 5 def_levels_byte_length,
      // 6 rep_levels_byte_length, 7 is_compressed
      int16_t dprev = 0;
      int16_t dfid = 0;
      uint8_t dtype = 0;
      bool dstop = false;
      while (OB_SUCC(ret) && !dstop) {
        if (OB_FAIL(tc.read_field_header(dprev, dfid, dtype))) {
          if (!tc.out_of_range_) {
            LOG_WARN("failed to read parquet v2 page header field", K(ret));
          }
        } else if (-1 == dfid) {
          dstop = true;
        } else if (1 == dfid) {
          int32_t v = 0;
          if (OB_FAIL(tc.read_i32(v))) {
            LOG_WARN("failed to read parquet v2 num values", K(ret));
          } else {
            info.v2_num_values_ = v;
          }
        } else if (4 == dfid) {
          int32_t v = 0;
          if (OB_FAIL(tc.read_i32(v))) {
            LOG_WARN("failed to read parquet v2 encoding", K(ret));
          } else {
            info.v2_encoding_ = v;
          }
        } else if (5 == dfid) {
          int32_t v = 0;
          if (OB_FAIL(tc.read_i32(v))) {
            LOG_WARN("failed to read parquet v2 def levels length", K(ret));
          } else {
            info.v2_def_len_ = v;
          }
        } else if (6 == dfid) {
          int32_t v = 0;
          if (OB_FAIL(tc.read_i32(v))) {
            LOG_WARN("failed to read parquet v2 rep levels length", K(ret));
          } else {
            info.v2_rep_len_ = v;
          }
        } else if (7 == dfid) {
          // a bool field inside a struct is encoded in the field type itself
          info.v2_is_compressed_ = (TT_TRUE == dtype);
        } else {
          if (OB_FAIL(tc.skip_value(dtype))) {
            LOG_WARN("failed to skip parquet v2 page header field", K(dfid), K(ret));
          }
        }
      }
    } else {
      if (OB_FAIL(tc.skip_value(ftype))) {
        LOG_WARN("failed to skip parquet page header field", K(fid), K(ret));
      }
    }
  }
  return ret;
}

int ParquetFileTableReader::decode_plain_from_(const char *buf, int64_t buf_len, int64_t &pos,
    int64_t &bit_pos, const LeafCol &leaf, ObObj &obj)
{
  int ret = OB_SUCCESS;
  switch (leaf.pq_type_) {
    case PQT_BOOLEAN: {
      const int64_t byte_idx = pos + (bit_pos >> 3);
      if (OB_UNLIKELY(byte_idx < 0 || byte_idx >= buf_len)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet boolean value out of range", K(byte_idx), K(buf_len), K(ret));
      } else {
        const uint8_t byte = static_cast<uint8_t>(buf[byte_idx]);
        const int32_t bit = (byte >> (bit_pos & 7)) & 1;
        bit_pos += 1;
        obj.set_bool(0 != bit);
      }
      break;
    }
    case PQT_INT32: {
      if (OB_UNLIKELY(pos + 4 > buf_len)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet int32 value truncated", K(pos), K(buf_len), K(ret));
      } else {
        const int32_t v = static_cast<int32_t>(
            static_cast<uint32_t>(static_cast<uint8_t>(buf[pos]))
            | (static_cast<uint32_t>(static_cast<uint8_t>(buf[pos + 1])) << 8)
            | (static_cast<uint32_t>(static_cast<uint8_t>(buf[pos + 2])) << 16)
            | (static_cast<uint32_t>(static_cast<uint8_t>(buf[pos + 3])) << 24));
        pos += 4;
        if (PCT_DATE == leaf.converted_type_) {
          obj.set_date(v);
        } else {
          obj.set_int(v);
        }
      }
      break;
    }
    case PQT_INT64: {
      if (OB_UNLIKELY(pos + 8 > buf_len)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet int64 value truncated", K(pos), K(buf_len), K(ret));
      } else {
        uint64_t u = 0;
        for (int64_t i = 0; i < 8; ++i) {
          u |= static_cast<uint64_t>(static_cast<uint8_t>(buf[pos + i])) << (8 * i);
        }
        const int64_t v = static_cast<int64_t>(u);
        pos += 8;
        if (PCT_TIMESTAMP_MILLIS == leaf.converted_type_) {
          obj.set_datetime(v * 1000);
        } else if (PCT_TIMESTAMP_MICROS == leaf.converted_type_) {
          obj.set_datetime(v);
        } else {
          obj.set_int(v);
        }
      }
      break;
    }
    case PQT_FLOAT: {
      if (OB_UNLIKELY(pos + 4 > buf_len)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet float value truncated", K(pos), K(buf_len), K(ret));
      } else {
        uint32_t u = 0;
        for (int64_t i = 0; i < 4; ++i) {
          u |= static_cast<uint32_t>(static_cast<uint8_t>(buf[pos + i])) << (8 * i);
        }
        float f = 0.0f;
        MEMCPY(&f, &u, sizeof(f));
        pos += 4;
        obj.set_double(static_cast<double>(f));
      }
      break;
    }
    case PQT_DOUBLE: {
      if (OB_UNLIKELY(pos + 8 > buf_len)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet double value truncated", K(pos), K(buf_len), K(ret));
      } else {
        uint64_t u = 0;
        for (int64_t i = 0; i < 8; ++i) {
          u |= static_cast<uint64_t>(static_cast<uint8_t>(buf[pos + i])) << (8 * i);
        }
        double d = 0.0;
        MEMCPY(&d, &u, sizeof(d));
        pos += 8;
        obj.set_double(d);
      }
      break;
    }
    case PQT_BYTE_ARRAY: {
      if (OB_UNLIKELY(pos + 4 > buf_len)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet byte array length truncated", K(pos), K(buf_len), K(ret));
      } else {
        const int64_t len = static_cast<int64_t>(
            static_cast<uint32_t>(static_cast<uint8_t>(buf[pos]))
            | (static_cast<uint32_t>(static_cast<uint8_t>(buf[pos + 1])) << 8)
            | (static_cast<uint32_t>(static_cast<uint8_t>(buf[pos + 2])) << 16)
            | (static_cast<uint32_t>(static_cast<uint8_t>(buf[pos + 3])) << 24));
        pos += 4;
        if (OB_UNLIKELY(len < 0 || pos + len > buf_len)) {
          ret = OB_INVALID_DATA;
          LOG_WARN("parquet byte array out of range", K(len), K(pos), K(buf_len), K(ret));
        } else {
          obj.set_varchar(ObString(static_cast<int32_t>(len), const_cast<char *>(buf + pos)));
          obj.meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
          pos += len;
        }
      }
      break;
    }
    case PQT_FIXED_LEN_BYTE_ARRAY: {
      const int64_t len = leaf.type_length_;
      if (OB_UNLIKELY(len < 0 || pos + len > buf_len)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("parquet fixed len byte array out of range", K(len), K(pos), K(buf_len), K(ret));
      } else {
        obj.set_varchar(ObString(static_cast<int32_t>(len), const_cast<char *>(buf + pos)));
        obj.meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        pos += len;
      }
      break;
    }
    default:
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported parquet value type", K(leaf.pq_type_), K(ret));
      break;
  }
  return ret;
}

int ParquetFileTableReader::rle_next_(const char *buf, int64_t buf_len, int64_t &pos,
    uint64_t &bit_acc, int32_t &bits, int64_t &run_left, bool &literal, uint64_t &repeat_val,
    int32_t bit_width, uint32_t &out)
{
  int ret = OB_SUCCESS;
  if (0 == run_left) {
    // read a new run header: varint (count << 1 | literal_flag)
    uint64_t h = 0;
    int32_t shift = 0;
    while (OB_SUCC(ret)) {
      if (pos >= buf_len) {
        ret = OB_INVALID_DATA;
        LOG_WARN("truncated rle run header", K(pos), K(buf_len), K(ret));
      } else {
        const uint8_t b = static_cast<uint8_t>(buf[pos++]);
        h |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (0 == (b & 0x80)) {
          break;
        }
        shift += 7;
        if (shift > 63) {
          ret = OB_INVALID_DATA;
          LOG_WARN("rle run header too long", K(ret));
        }
      }
    }
    if (OB_SUCC(ret)) {
      run_left = static_cast<int64_t>(h >> 1);
      literal = (0 != (h & 1));
      if (literal) {
        bit_acc = 0;
        bits = 0;
      } else {
        const int32_t nbytes = (bit_width + 7) / 8;
        repeat_val = 0;
        if (OB_UNLIKELY(pos + nbytes > buf_len)) {
          ret = OB_INVALID_DATA;
          LOG_WARN("truncated rle repeat value", K(pos), K(nbytes), K(buf_len), K(ret));
        } else {
          for (int32_t i = 0; i < nbytes; ++i) {
            repeat_val |= static_cast<uint64_t>(static_cast<uint8_t>(buf[pos++])) << (8 * i);
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    --run_left;
    if (literal) {
      if (0 == bit_width) {
        out = 0;
      } else {
        while (OB_SUCC(ret) && bits < bit_width) {
          if (pos >= buf_len) {
            ret = OB_INVALID_DATA;
            LOG_WARN("truncated rle literal run", K(pos), K(buf_len), K(ret));
          } else {
            bit_acc |= static_cast<uint64_t>(static_cast<uint8_t>(buf[pos++])) << bits;
            bits += 8;
          }
        }
        if (OB_SUCC(ret)) {
          out = static_cast<uint32_t>(bit_acc & ((1ULL << bit_width) - 1));
          bit_acc >>= bit_width;
          bits -= bit_width;
        }
      }
    } else {
      out = static_cast<uint32_t>(repeat_val);
    }
  }
  return ret;
}

int ParquetFileTableReader::fetch_value_(ColCursor &cur, int64_t col_idx, ObObj &obj,
    bool &fetched)
{
  int ret = OB_SUCCESS;
  fetched = false;
  const ChunkMeta &chunk = chunks_.at(row_groups_.at(cur_row_group_).first_chunk_idx_ + col_idx);
  const LeafCol &leaf = leaf_cols_.at(col_idx);
  if (OB_UNLIKELY(cur.values_left_ <= 0)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("parquet column values are exhausted", K(col_idx), K(ret));
  } else {
    if (cur.page_values_left_ <= 0) {
      bool has_page = false;
      while (OB_SUCC(ret) && !has_page) {
        if (OB_FAIL(load_next_page_(cur, col_idx, has_page))) {
          LOG_WARN("failed to load parquet page", K(col_idx), K(ret));
        } else if (!has_page
                   && (cur.next_page_offset_ < 0
                       || cur.next_page_offset_ >= file_stat_.file_size_)) {
          ret = OB_INVALID_DATA;
          LOG_WARN("parquet page stream ended before all values were consumed", K(col_idx),
              K(cur.values_left_), K(ret));
        }
        // otherwise an index page (or a dictionary page) was consumed, retry
      }
    }
    if (OB_SUCC(ret)) {
      --cur.page_values_left_;
      --cur.values_left_;
      if (chunk.max_def_level_ > 0) {
        if (OB_UNLIKELY(cur.def_consumed_ < 0 || cur.def_consumed_ >= cur.def_levels_cap_)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("bad parquet def level cursor", K(col_idx), K(cur.def_consumed_), K(ret));
        } else {
          // the definition levels of the current page were materialized at
          // page load time; each fetched value consumes one level in order
          const uint8_t def = cur.def_levels_[cur.def_consumed_++];
          if (0 == def) {
            obj.set_null();
            fetched = true;
          }
        }
      }
    }
    if (OB_SUCC(ret) && !fetched) {
      if (PE_PLAIN == cur.cur_encoding_) {
        if (OB_FAIL(decode_plain_from_(cur.page_buf_, cur.page_len_, cur.val_pos_,
                cur.bool_bit_pos_, leaf, obj))) {
          LOG_WARN("failed to decode parquet plain value", K(col_idx), K(ret));
        } else {
          fetched = true;
        }
      } else if (PE_PLAIN_DICTIONARY == cur.cur_encoding_
                 || PE_RLE_DICTIONARY == cur.cur_encoding_) {
        if (OB_UNLIKELY(!cur.dict_ready_)) {
          ret = OB_INVALID_DATA;
          LOG_WARN("parquet dictionary page is missing", K(col_idx), K(ret));
        } else {
          if (!cur.irle_started_) {
            // the first byte of the value section is the index bit width
            if (OB_UNLIKELY(cur.val_pos_ + 1 > cur.page_len_)) {
              ret = OB_INVALID_DATA;
              LOG_WARN("parquet dictionary bit width is missing", K(col_idx), K(ret));
            } else {
              cur.dict_bit_width_ = static_cast<int32_t>(static_cast<uint8_t>(cur.page_buf_[cur.val_pos_]));
              cur.val_pos_ += 1;
              cur.irle_pos_ = cur.val_pos_;
              cur.irle_started_ = true;
            }
          }
          if (OB_SUCC(ret)) {
            uint32_t idx = 0;
            if (OB_UNLIKELY(cur.dict_bit_width_ < 0 || cur.dict_bit_width_ > 32)) {
              ret = OB_INVALID_DATA;
              LOG_WARN("bad parquet dictionary bit width", K(cur.dict_bit_width_), K(ret));
            } else if (OB_FAIL(rle_next_(cur.page_buf_, cur.page_len_, cur.irle_pos_,
                            cur.irle_bit_acc_, cur.irle_bits_, cur.irle_run_left_,
                            cur.irle_literal_, cur.irle_repeat_val_, cur.dict_bit_width_, idx))) {
              LOG_WARN("failed to read parquet dictionary index", K(col_idx), K(ret));
            } else if (OB_UNLIKELY(idx >= cur.dict_count_)) {
              ret = OB_INVALID_DATA;
              LOG_WARN("parquet dictionary index out of range", K(idx), K(cur.dict_count_),
                  K(ret));
            } else {
              obj = cur.dict_objs_[idx];
              fetched = true;
            }
          }
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unsupported parquet value encoding", K(cur.cur_encoding_), K(ret));
      }
    }
  }
  return ret;
}

int ParquetFileTableReader::get_next_row(ObIArray<ObObj> &row, bool &has_next)
{
  int ret = OB_SUCCESS;
  has_next = false;
  row.reuse();
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("parquet file table reader is not inited", K(ret));
  } else if (options_.max_rows_ > 0 && rows_emitted_ >= options_.max_rows_) {
    // max rows reached
  } else if (cur_row_group_ >= row_groups_.count()) {
    // iteration end
  } else {
    if (!rg_opened_) {
      if (OB_FAIL(open_row_group_(cur_row_group_))) {
        LOG_WARN("failed to open parquet row group", K(cur_row_group_), K(ret));
      } else {
        rg_opened_ = true;
      }
    }
    if (OB_SUCC(ret)) {
      const RowGroupMeta &rg = row_groups_.at(cur_row_group_);
      for (int64_t col = 0; OB_SUCC(ret) && col < leaf_cols_.count(); ++col) {
        ObObj obj;
        if (!projected_.at(col) || !cursors_[col].active_) {
          obj.set_null();
        } else {
          bool fetched = false;
          if (OB_FAIL(fetch_value_(cursors_[col], col, obj, fetched))) {
            LOG_USER_ERROR(OB_INVALID_DATA);
            LOG_WARN("failed to fetch parquet value", K(path_), K(col), K(ret));
          } else if (OB_UNLIKELY(!fetched)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("parquet value was not fetched", K(col), K(ret));
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(row.push_back(obj))) {
          LOG_WARN("failed to push parquet row cell", K(col), K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        ++rows_emitted_;
        ++row_group_row_;
        has_next = true;
        if (row_group_row_ >= rg.num_rows_) {
          ++cur_row_group_;
          row_group_row_ = 0;
          rg_opened_ = false;
        }
      }
    }
  }
  return ret;
}

int ParquetFileTableReader::reuse()
{
  int ret = OB_SUCCESS;
  if (inited_) {
    cur_row_group_ = 0;
    row_group_row_ = 0;
    rg_opened_ = false;
    rows_emitted_ = 0;
    reset_cursors_();
  }
  return ret;
}

int ParquetFileTableReader::get_file_stat(FileTableFileStat &stat) const
{
  int ret = OB_SUCCESS;
  stat = FileTableFileStat();
  if (OB_UNLIKELY(path_.empty())) {
    // not inited yet, return empty stat
  } else {
#ifdef _WIN32
    struct __stat64 st;
    if (0 != ::_stat64(path_.ptr(), &st)) {
#else
    struct stat st;
    if (0 != ::stat(path_.ptr(), &st)) {
#endif
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to stat parquet file", K(path_), K(ret));
    } else {
      stat.file_size_ = static_cast<int64_t>(st.st_size);
      stat.last_modify_time_ = static_cast<int64_t>(st.st_mtime);
    }
  }
  return ret;
}

void ParquetFileTableReader::close()
{
  if (OB_NOT_NULL(fp_)) {
    ::fclose(fp_);
    fp_ = NULL;
  }
  if (OB_NOT_NULL(hdr_buf_)) {
    alloc_.free(hdr_buf_);
    hdr_buf_ = NULL;
  }
  if (OB_NOT_NULL(cursors_)) {
    for (int64_t i = 0; i < cursor_count_; ++i) {
      ColCursor &cur = cursors_[i];
      free_buf_(cur.page_comp_buf_, cur.page_comp_cap_);
      free_buf_(cur.page_buf_, cur.page_cap_);
      free_buf_(cur.dict_buf_, cur.dict_cap_);
      if (OB_NOT_NULL(cur.def_levels_)) {
        alloc_.free(cur.def_levels_);
        cur.def_levels_ = NULL;
      }
      cur.def_levels_cap_ = 0;
      if (OB_NOT_NULL(cur.dict_objs_)) {
        for (int64_t k = 0; k < cur.dict_count_; ++k) {
          cur.dict_objs_[k].~ObObj();
        }
        alloc_.free(cur.dict_objs_);
        cur.dict_objs_ = NULL;
      }
      cur.dict_count_ = 0;
    }
    alloc_.free(cursors_);
    cursors_ = NULL;
  }
  cursor_count_ = 0;
  inited_ = false;
}

} // end namespace sql
} // end namespace oceanbase
