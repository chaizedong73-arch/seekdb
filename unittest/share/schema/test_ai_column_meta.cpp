/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include "share/schema/ob_ai_column_meta.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
using namespace common;

TEST(AIColumnMeta, round_trip_multiple_dependencies_and_escaped_expression)
{
  ObArenaAllocator allocator("AIColMetaTest");
  AIColumnMeta original;
  original.func_ = "ai_complete";
  original.expr_ = "AI_COMPLETE('model', concat('quote: \"', title, body, category))";
  original.model_id_ = "model";
  original.sync_mode_ = "async";
  ASSERT_EQ(OB_SUCCESS, original.dep_cols_.push_back(ObString("title")));
  ASSERT_EQ(OB_SUCCESS, original.dep_cols_.push_back(ObString("body")));
  ASSERT_EQ(OB_SUCCESS, original.dep_cols_.push_back(ObString("category")));
  ObString serialized;
  ASSERT_EQ(OB_SUCCESS, build_ai_column_meta_json(allocator, original, serialized));
  AIColumnMeta decoded;
  ASSERT_EQ(OB_SUCCESS, parse_ai_column_meta(allocator, serialized, decoded));
  EXPECT_EQ(original.expr_, decoded.expr_);
  ASSERT_EQ(3, decoded.dep_cols_.count());
  for (int64_t i = 0; i < original.dep_cols_.count(); ++i) {
    EXPECT_EQ(original.dep_cols_.at(i), decoded.dep_cols_.at(i));
  }
  ASSERT_EQ(OB_SUCCESS, parse_ai_column_meta(allocator, serialized, decoded));
  EXPECT_EQ(3, decoded.dep_cols_.count());
}
} // namespace schema
} // namespace share
} // namespace oceanbase
