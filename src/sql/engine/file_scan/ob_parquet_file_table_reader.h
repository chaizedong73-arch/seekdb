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

#include <stdio.h>

#include "sql/engine/file_scan/file_table_reader.h"

namespace oceanbase
{
namespace sql
{

/**
 * @brief Self-contained parquet file table reader (no third party deps).
 * Supported subset:
 * - flat schemas only (the root schema elements must all be leaves, nested
 *   groups and REPEATED fields are rejected);
 * - thrift compact protocol footer parsing (FileMetaData / SchemaElement /
 *   RowGroup / ColumnChunk / ColumnMetaData / PageHeader);
 * - data pages v1 and v2, PLAIN, PLAIN_DICTIONARY and RLE_DICTIONARY value
 *   encodings, RLE/bit-packed definition levels (optional columns);
 * - UNCOMPRESSED pages plus a built-in snappy decompressor;
 * - types BOOLEAN, INT32, INT64, FLOAT, DOUBLE, BYTE_ARRAY and
 *   FIXED_LEN_BYTE_ARRAY, with converted types DATE (int32 days) and
 *   TIMESTAMP_MILLIS/MICROS (int64) mapped to OB DATE/DATETIME;
 * - row groups are iterated sequentially, and set_projection() prunes the
 *   columns that are actually decoded (unselected columns output null).
 */
class ParquetFileTableReader : public IFileTableReader
{
public:
  explicit ParquetFileTableReader(common::ObIAllocator &alloc);
  virtual ~ParquetFileTableReader();

  virtual int init(const common::ObString &path, const FileReadOptions &options) override;
  virtual int get_column_metas(common::ObIArray<FileColumnMeta> &columns) const override;
  virtual int get_next_row(common::ObIArray<common::ObObj> &row, bool &has_next) override;
  virtual int reuse() override;
  virtual int get_file_stat(FileTableFileStat &stat) const override;
  virtual void close() override;
  virtual int set_projection(const common::ObBitSet<> &col_idxs) override;

private:
  // parquet spec enums (subset)
  enum ParquetType
  {
    PQT_BOOLEAN = 0,
    PQT_INT32 = 1,
    PQT_INT64 = 2,
    PQT_INT96 = 3,
    PQT_FLOAT = 4,
    PQT_DOUBLE = 5,
    PQT_BYTE_ARRAY = 6,
    PQT_FIXED_LEN_BYTE_ARRAY = 7,
  };
  enum ParquetConvertedType
  {
    PCT_NONE = -1,
    PCT_UTF8 = 0,
    PCT_DATE = 6,
    PCT_TIME_MILLIS = 7,
    PCT_TIME_MICROS = 8,
    PCT_TIMESTAMP_MILLIS = 9,
    PCT_TIMESTAMP_MICROS = 10,
  };
  enum ParquetEncoding
  {
    PE_PLAIN = 0,
    PE_PLAIN_DICTIONARY = 2,
    PE_RLE = 3,
    PE_BIT_PACKED = 4,
    PE_RLE_DICTIONARY = 8,
  };
  enum ParquetCodec
  {
    PC_UNCOMPRESSED = 0,
    PC_SNAPPY = 1,
  };
  enum ParquetPageType
  {
    PP_DATA_PAGE = 0,
    PP_INDEX_PAGE = 1,
    PP_DICTIONARY_PAGE = 2,
    PP_DATA_PAGE_V2 = 3,
  };
  enum FieldRepetitionType
  {
    FRT_REQUIRED = 0,
    FRT_OPTIONAL = 1,
    FRT_REPEATED = 2,
  };
  // thrift compact protocol value types
  enum ThriftType
  {
    TT_STOP = 0,
    TT_TRUE = 1,
    TT_FALSE = 2,
    TT_BYTE = 3,
    TT_I16 = 4,
    TT_I32 = 5,
    TT_I64 = 6,
    TT_DOUBLE = 7,
    TT_BINARY = 8,
    TT_LIST = 9,
    TT_SET = 10,
    TT_MAP = 11,
    TT_STRUCT = 12,
  };

  // cursor over a thrift compact protocol byte buffer
  struct ThriftCursor
  {
    ThriftCursor() : buf_(NULL), len_(0), pos_(0), out_of_range_(false)
    {}
    void reset(const char *buf, int64_t len)
    {
      buf_ = buf;
      len_ = len;
      pos_ = 0;
      out_of_range_ = false;
    }
    int read_byte(uint8_t &v);
    int read_uleb128(uint64_t &v);
    int read_i16(int16_t &v);
    int read_i32(int32_t &v);
    int read_i64(int64_t &v);
    int read_binary(common::ObString &s);
    int read_list_header(uint8_t &etype, int64_t &size);
    // fid = -1 means the stop field is reached
    int read_field_header(int16_t &prev_fid, int16_t &fid, uint8_t &type);
    int skip_value(uint8_t type);

    const char *buf_;
    int64_t len_;
    int64_t pos_;
    bool out_of_range_;
  };

  // one leaf column of the (flat) parquet schema
  struct LeafCol
  {
    LeafCol() : pq_type_(PQT_INT32), converted_type_(PCT_NONE), repetition_(FRT_REQUIRED),
                type_length_(0)
    {}
    common::ObString name_;
    int32_t pq_type_;
    int32_t converted_type_;
    int32_t repetition_;
    int32_t type_length_;
    TO_STRING_KV(K_(name), K_(pq_type), K_(converted_type), K_(repetition), K_(type_length));
  };

  // one column chunk (column x row group)
  struct ChunkMeta
  {
    ChunkMeta() : data_start_offset_(-1), total_compressed_size_(0), num_values_(0),
                  codec_(PC_UNCOMPRESSED), max_def_level_(0)
    {}
    int64_t data_start_offset_;
    int64_t total_compressed_size_;
    int64_t num_values_;
    int32_t codec_;
    int32_t max_def_level_;
    TO_STRING_KV(K_(data_start_offset), K_(total_compressed_size), K_(num_values),
                 K_(codec), K_(max_def_level));
  };

  struct RowGroupMeta
  {
    RowGroupMeta() : num_rows_(0), first_chunk_idx_(0)
    {}
    int64_t num_rows_;
    int64_t first_chunk_idx_;
    TO_STRING_KV(K_(num_rows), K_(first_chunk_idx));
  };

  struct PageHeaderInfo
  {
    PageHeaderInfo()
        : page_type_(0), uncompressed_size_(0), compressed_size_(0), has_v1_(false),
          v1_num_values_(0), v1_encoding_(PE_PLAIN), has_dict_(false), dict_num_values_(0),
          dict_encoding_(PE_PLAIN), has_v2_(false), v2_num_values_(0), v2_encoding_(PE_PLAIN),
          v2_def_len_(0), v2_rep_len_(0), v2_is_compressed_(true)
    {}
    int32_t page_type_;
    int32_t uncompressed_size_;
    int32_t compressed_size_;
    bool has_v1_;
    int64_t v1_num_values_;
    int32_t v1_encoding_;
    bool has_dict_;
    int64_t dict_num_values_;
    int32_t dict_encoding_;
    bool has_v2_;
    int64_t v2_num_values_;
    int32_t v2_encoding_;
    int32_t v2_def_len_;
    int32_t v2_rep_len_;
    bool v2_is_compressed_;
  };

  // per column runtime state; only projected columns are active in a row group
  struct ColCursor
  {
    ColCursor()
        : active_(false), next_page_offset_(-1), values_left_(0), page_values_left_(0),
          page_comp_buf_(NULL), page_comp_cap_(0), page_buf_(NULL), page_cap_(0), page_len_(0),
          lv_ptr_(NULL), lv_len_(0), cur_encoding_(PE_PLAIN), val_pos_(0), bool_bit_pos_(0),
          lrle_pos_(0), lrle_bit_acc_(0), lrle_bits_(0), lrle_run_left_(0), lrle_literal_(false),
          lrle_repeat_val_(0), def_bit_width_(0), def_levels_(NULL), def_levels_cap_(0),
          def_consumed_(0), dict_ready_(false), dict_count_(0), dict_objs_(NULL), dict_buf_(NULL),
          dict_cap_(0), dict_len_(0), dict_bit_width_(0), irle_pos_(0), irle_bit_acc_(0),
          irle_bits_(0), irle_run_left_(0), irle_literal_(false), irle_repeat_val_(0),
          irle_started_(false)
    {}
    bool active_;
    // page stream position
    int64_t next_page_offset_;
    int64_t values_left_;
    int64_t page_values_left_;
    // page buffers (v1/v2 data pages)
    char *page_comp_buf_;
    int64_t page_comp_cap_;
    char *page_buf_;
    int64_t page_cap_;
    int64_t page_len_;
    // definition level rle stream source (v1: page_buf_, v2: page_comp_buf_)
    const char *lv_ptr_;
    int64_t lv_len_;
    int32_t cur_encoding_;
    // plain value decoding cursor inside page_buf_
    int64_t val_pos_;
    int64_t bool_bit_pos_;
    // level rle state (the stream is fully materialized at page load time)
    int64_t lrle_pos_;
    uint64_t lrle_bit_acc_;
    int32_t lrle_bits_;
    int64_t lrle_run_left_;
    bool lrle_literal_;
    uint64_t lrle_repeat_val_;
    int32_t def_bit_width_;
    // materialized definition levels of the current page
    uint8_t *def_levels_;
    int64_t def_levels_cap_;
    int64_t def_consumed_;
    // dictionary (kept in a dedicated buffer: data pages reuse page_buf_)
    bool dict_ready_;
    int64_t dict_count_;
    common::ObObj *dict_objs_;
    char *dict_buf_;
    int64_t dict_cap_;
    int64_t dict_len_;
    int32_t dict_bit_width_;
    // dictionary index rle state
    int64_t irle_pos_;
    uint64_t irle_bit_acc_;
    int32_t irle_bits_;
    int64_t irle_run_left_;
    bool irle_literal_;
    uint64_t irle_repeat_val_;
    bool irle_started_;
  };

private:
  int dup_string_(const common::ObString &src, common::ObString &dst);
  int alloc_buf_(char *&buf, int64_t &cap, int64_t need);
  int free_buf_(char *&buf, int64_t &cap);
  // footer
  int read_footer_();
  int parse_file_meta_(ThriftCursor &tc);
  int parse_schema_element_(ThriftCursor &tc, LeafCol &elem, int64_t &num_children);
  int parse_row_group_(ThriftCursor &tc);
  int parse_column_chunk_(ThriftCursor &tc, ChunkMeta &chunk, int64_t leaf_idx);
  int parse_page_header_(ThriftCursor &tc, PageHeaderInfo &info);
  // schema mapping
  int map_leaf_type_(const LeafCol &leaf, common::ObObjMeta &type) const;
  // row group iteration
  int open_row_group_(int64_t rg_idx);
  int load_next_page_(ColCursor &cur, int64_t col_idx, bool &has_page);
  int decode_dictionary_page_(ColCursor &cur, int64_t col_idx, const PageHeaderInfo &info);
  int decode_plain_from_(const char *buf, int64_t buf_len, int64_t &pos, int64_t &bit_pos,
      const LeafCol &leaf, common::ObObj &obj);
  int rle_next_(const char *buf, int64_t buf_len, int64_t &pos, uint64_t &bit_acc, int32_t &bits,
      int64_t &run_left, bool &literal, uint64_t &repeat_val, int32_t bit_width, uint32_t &out);
  int fetch_value_(ColCursor &cur, int64_t col_idx, common::ObObj &obj, bool &fetched);
  void reset_cursors_();

private:
  static const int64_t HDR_PRE_READ_SIZE = 8LL * 1024;
  static const int64_t HDR_MAX_READ_SIZE = 1LL << 20;
  static const int64_t FOOTER_MAGIC_SIZE = 8;

private:
  common::ObIAllocator &alloc_;
  common::ObString path_;
  FileReadOptions options_;
  common::ObSEArray<FileColumnMeta, 16> columns_;
  common::ObSEArray<LeafCol, 16> leaf_cols_;
  common::ObSEArray<RowGroupMeta, 8> row_groups_;
  common::ObSEArray<ChunkMeta, 64> chunks_;
  common::ObSEArray<bool, 16> projected_;
  ColCursor *cursors_;
  int64_t cursor_count_;
  int64_t cur_row_group_;
  int64_t row_group_row_;
  bool rg_opened_;
  int64_t rows_emitted_;
  ::FILE *fp_;
  char *hdr_buf_;
  FileTableFileStat file_stat_;
  bool inited_;
};

} // end namespace sql
} // end namespace oceanbase
