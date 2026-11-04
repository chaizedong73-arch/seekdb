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

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

// AI column function names (lower case, case-insensitive comparison)
static const char AI_FUNC_NAME_AI_COMPLETE[] = "ai_complete";
static const char AI_FUNC_NAME_AI_EMBED[] = "ai_embed";
// AI column option default values
static const char AI_COLUMN_DEFAULT_SYNC_MODE[] = "async";
static const int64_t AI_COLUMN_DEFAULT_MAX_RETRIES = 3;

// In-memory representation of the AI COLUMN definition of a table column.
// The definition is persisted as a JSON text in the column schema
// cur_default_value field, see build_ai_column_meta_json().
struct AIColumnMeta
{
  AIColumnMeta()
      : func_(), expr_(), model_id_(), sync_mode_(), max_retries_(AI_COLUMN_DEFAULT_MAX_RETRIES),
        def_version_(1), dep_cols_()
  {}
  common::ObString func_;       // ai_complete / ai_embed
  common::ObString expr_;       // full AI expression text, e.g. AI_COMPLETE('m', ...)
  common::ObString model_id_;   // first argument of the AI function
  common::ObString sync_mode_;  // only 'async' is supported
  int64_t max_retries_;
  int64_t def_version_;
  // Note: strings of dep_cols_ point into the json text memory, caller must
  // keep the json text alive while using this struct, or deep copy them.
  common::ObSEArray<common::ObString, 4> dep_cols_;

  void reset()
  {
    func_.reset();
    expr_.reset();
    model_id_.reset();
    sync_mode_.reset();
    max_retries_ = AI_COLUMN_DEFAULT_MAX_RETRIES;
    def_version_ = 1;
    dep_cols_.reset();
  }
  bool is_valid() const { return !func_.empty() && !expr_.empty(); }
  bool is_complete_func() const
  { return func_.case_compare(AI_FUNC_NAME_AI_COMPLETE) == 0; }
  bool is_embed_func() const
  { return func_.case_compare(AI_FUNC_NAME_AI_EMBED) == 0; }
  bool is_async_mode() const
  { return sync_mode_.case_compare(AI_COLUMN_DEFAULT_SYNC_MODE) == 0; }
  bool has_dep_column(const common::ObString &col_name) const;
};

// Serialize the AI column meta to a json text. All strings are deep copied or
// escaped by the json writer, so the input meta can live on stack.
int build_ai_column_meta_json(
    common::ObIAllocator &allocator,
    const AIColumnMeta &meta,
    common::ObString &json_str);

// Parse the AI column meta from the json text produced by
// build_ai_column_meta_json(). All parsed strings (including dep_cols_)
// point into the memory allocated from the given allocator.
int parse_ai_column_meta(
    common::ObIAllocator &allocator,
    const common::ObString &json_str,
    AIColumnMeta &meta);

} // namespace schema
} // namespace share
} // namespace oceanbase
