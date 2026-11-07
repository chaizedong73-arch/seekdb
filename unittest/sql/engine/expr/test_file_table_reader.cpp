/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "lib/allocator/page_arena.h"
#include "sql/engine/file_scan/ob_csv_file_table_reader.h"
#include "sql/engine/file_scan/ob_json_file_table_reader.h"
#include "sql/engine/file_scan/ob_parquet_file_table_reader.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

TEST(FileTableReaderOptions, full_inference_and_validation)
{
  FileReadOptions options;
  ASSERT_TRUE(options.parse_header_);
  ASSERT_EQ(-1, options.sample_size_);
  ObSEArray<ObObj, 8> args;
  ObObj value;
  value.set_varchar("/tmp/input.csv");
  ASSERT_EQ(OB_SUCCESS, args.push_back(value));
  value.set_varchar("sample_size");
  ASSERT_EQ(OB_SUCCESS, args.push_back(value));
  value.set_int(-1);
  ASSERT_EQ(OB_SUCCESS, args.push_back(value));
  ASSERT_EQ(OB_SUCCESS, parse_file_read_options(args, options));
  args.at(2).set_int(10);
  ASSERT_EQ(OB_NOT_SUPPORTED, parse_file_read_options(args, options));
  args.at(2).set_int(-1);
  value.set_varchar("SAMPLE_SIZE");
  ASSERT_EQ(OB_SUCCESS, args.push_back(value));
  value.set_int(-1);
  ASSERT_EQ(OB_SUCCESS, args.push_back(value));
  ASSERT_EQ(OB_INVALID_ARGUMENT, parse_file_read_options(args, options));
}

TEST(FileTableReaderCsv, duplicate_headers_and_multiline_quotes)
{
  char path[] = "/tmp/seekdb-csv-reader-XXXXXX";
  const int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  FILE *file = fdopen(fd, "wb");
  ASSERT_NE(nullptr, file);
  const char input[] = "id,note,id\n1,\"first\nsecond\",2\n3,\"\",4\n";
  EXPECT_EQ(sizeof(input) - 1, fwrite(input, 1, sizeof(input) - 1, file));
  ASSERT_EQ(0, fclose(file));
  ObArenaAllocator allocator(ObModIds::TEST);
  CsvFileTableReader reader(allocator);
  FileReadOptions options;
  const int init_ret = reader.init(ObString(path), options);
  unlink(path);
  ASSERT_EQ(OB_SUCCESS, init_ret);
  ObSEArray<FileColumnMeta, 8> columns;
  ASSERT_EQ(OB_SUCCESS, reader.get_column_metas(columns));
  ASSERT_EQ(3, columns.count());
  EXPECT_EQ(ObString("id__2"), columns.at(2).name_);
  ObSEArray<ObObj, 8> row;
  bool has_next = false;
  ASSERT_EQ(OB_SUCCESS, reader.get_next_row(row, has_next));
  ASSERT_TRUE(has_next);
  EXPECT_EQ(1, row.at(0).get_int());
  EXPECT_EQ(ObString("first\nsecond"), row.at(1).get_string());
  row.reuse();
  ASSERT_EQ(OB_SUCCESS, reader.get_next_row(row, has_next));
  ASSERT_TRUE(has_next);
  EXPECT_FALSE(row.at(1).is_null());
  EXPECT_TRUE(row.at(1).get_string().empty());
  reader.close();
}
TEST(FileTableReaderJson, union_schema_and_missing_values)
{
  char path[] = "/tmp/seekdb-json-reader-XXXXXX";
  const int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  FILE *file = fdopen(fd, "wb");
  ASSERT_NE(nullptr, file);
  const char input[] = "{\"id\":1,\"name\":\"Alpha\"}\n{\"id\":2,\"score\":2.5}\n";
  EXPECT_EQ(sizeof(input) - 1, fwrite(input, 1, sizeof(input) - 1, file));
  ASSERT_EQ(0, fclose(file));
  ObArenaAllocator allocator(ObModIds::TEST);
  JsonFileTableReader reader(allocator, FILE_TABLE_NDJSON);
  FileReadOptions options;
  const int init_ret = reader.init(ObString(path), options);
  unlink(path);
  ASSERT_EQ(OB_SUCCESS, init_ret);
  ObSEArray<FileColumnMeta, 8> columns;
  ASSERT_EQ(OB_SUCCESS, reader.get_column_metas(columns));
  ASSERT_EQ(3, columns.count());
  ObSEArray<ObObj, 8> row;
  bool has_next = false;
  ASSERT_EQ(OB_SUCCESS, reader.get_next_row(row, has_next));
  ASSERT_TRUE(has_next);
  EXPECT_EQ(ObString("Alpha"), row.at(1).get_string());
  EXPECT_TRUE(row.at(2).is_null());
  row.reuse();
  ASSERT_EQ(OB_SUCCESS, reader.get_next_row(row, has_next));
  ASSERT_TRUE(has_next);
  EXPECT_EQ(2, row.at(0).get_int());
  EXPECT_TRUE(row.at(1).is_null());
  EXPECT_DOUBLE_EQ(2.5, row.at(2).get_double());
  reader.close();
}
TEST(FileTableReaderJson, rejects_invalid_json_syntax)
{
  const char *inputs[] = {"{\"n\":01}", "{\"n\":+1}", "{\"n\":1,}",
                          "{\"s\":\"\x01\"}", "{\"s\":\"\\uDC00\"}"};
  for (const char *input : inputs) {
    SCOPED_TRACE(input);
    char path[] = "/tmp/seekdb-bad-json-XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    FILE *file = fdopen(fd, "wb");
    ASSERT_NE(nullptr, file);
    EXPECT_EQ(strlen(input), fwrite(input, 1, strlen(input), file));
    ASSERT_EQ(0, fclose(file));
    ObArenaAllocator allocator(ObModIds::TEST);
    JsonFileTableReader reader(allocator, FILE_TABLE_NDJSON);
    FileReadOptions options;
    const int init_ret = reader.init(ObString(path), options);
    unlink(path);
    EXPECT_EQ(OB_INVALID_DATA, init_ret);
    reader.close();
  }
}

TEST(FileTableReaderTypes, date_widens_to_datetime)
{
  FileCellKind merged = FCK_DATE;
  ASSERT_EQ(OB_SUCCESS, merge_file_cell_kind(merged, FCK_DATETIME));
  ASSERT_EQ(FCK_DATETIME, merged);
  ObObjMeta type;
  ASSERT_EQ(OB_SUCCESS, file_cell_kind_to_type(merged, type));
  ObObj value;
  ASSERT_EQ(OB_SUCCESS, file_cell_text_to_obj(ObString("2026-11-07"), type, FCK_DATE, value));
  EXPECT_EQ(ObDateTimeType, value.get_type());
}
TEST(FileTableReaderParquet, rejects_oversized_footer_before_allocating_it)
{
  char path[] = "/tmp/seekdb-parquet-limit-XXXXXX";
  const int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  const uint32_t footer_size = 300U << 20;
  ASSERT_EQ(0, ftruncate(fd, static_cast<off_t>(footer_size) + 12));
  EXPECT_EQ(4, pwrite(fd, "PAR1", 4, 0));
  char tail[8] = {};
  for (int i = 0; i < 4; ++i) { tail[i] = static_cast<char>(footer_size >> (8 * i)); }
  memcpy(tail + 4, "PAR1", 4);
  EXPECT_EQ(8, pwrite(fd, tail, sizeof(tail), static_cast<off_t>(footer_size) + 4));
  ASSERT_EQ(0, close(fd));
  ObArenaAllocator allocator(ObModIds::TEST);
  ParquetFileTableReader reader(allocator);
  FileReadOptions options;
  const int init_ret = reader.init(ObString(path), options);
  unlink(path);
  EXPECT_EQ(OB_SIZE_OVERFLOW, init_ret);
  reader.close();
}
} // namespace sql
} // namespace oceanbase
