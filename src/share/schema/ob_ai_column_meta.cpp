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

#define USING_LOG_PREFIX SHARE

#include "share/schema/ob_ai_column_meta.h"

#include "common/json_type/ob_json_tree.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_common.h"
#include "share/ob_define.h"

namespace oceanbase
{
using namespace common;
namespace share
{
namespace schema
{

namespace
{
int copy_json_string(
    ObIAllocator &allocator,
    const ObString &src,
    ObString &dst)
{
  int ret = OB_SUCCESS;
  if (src.empty()) {
    dst.reset();
  } else {
    char *buf = static_cast<char *>(allocator.alloc(src.length() + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to alloc memory for ai column meta string", K(ret), K(src.length()));
    } else {
      MEMCPY(buf, src.ptr(), src.length());
      buf[src.length()] = '\0';
      dst.assign_ptr(buf, static_cast<int32_t>(src.length()));
    }
  }
  return ret;
}
} // anonymous namespace

bool AIColumnMeta::has_dep_column(const common::ObString &col_name) const
{
  bool bret = false;
  for (int64_t i = 0; !bret && i < dep_cols_.count(); ++i) {
    bret = (dep_cols_.at(i).case_compare(col_name) == 0);
  }
  return bret;
}

int build_ai_column_meta_json(
    ObIAllocator &allocator,
    const AIColumnMeta &meta,
    ObString &json_str)
{
  int ret = OB_SUCCESS;
  json::ObJsonObject obj(&allocator);
  json::ObJsonString func_node(meta.func_);
  json::ObJsonString expr_node(meta.expr_);
  json::ObJsonString model_node(meta.model_id_);
  json::ObJsonString sync_node(meta.sync_mode_);
  json::ObJsonInt retry_node(meta.max_retries_);
  json::ObJsonInt version_node(meta.def_version_);
  if (OB_FAIL(obj.add(ObString("func"), &func_node))) {
    LOG_WARN("fail to add func to ai column meta json", K(ret));
  } else if (OB_FAIL(obj.add(ObString("expr"), &expr_node))) {
    LOG_WARN("fail to add expr to ai column meta json", K(ret));
  } else if (OB_FAIL(obj.add(ObString("model"), &model_node))) {
    LOG_WARN("fail to add model to ai column meta json", K(ret));
  } else if (OB_FAIL(obj.add(ObString("sync_mode"), &sync_node))) {
    LOG_WARN("fail to add sync_mode to ai column meta json", K(ret));
  } else if (OB_FAIL(obj.add(ObString("max_retries"), &retry_node))) {
    LOG_WARN("fail to add max_retries to ai column meta json", K(ret));
  } else if (OB_FAIL(obj.add(ObString("def_version"), &version_node))) {
    LOG_WARN("fail to add def_version to ai column meta json", K(ret));
  } else {
    json::ObJsonArray dep_array(&allocator);
    for (int64_t i = 0; OB_SUCC(ret) && i < meta.dep_cols_.count(); ++i) {
      json::ObJsonString dep_node(meta.dep_cols_.at(i));
      if (OB_FAIL(dep_array.append(&dep_node))) {
        LOG_WARN("fail to append dep column to ai column meta json", K(ret));
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(obj.add(ObString("deps"), &dep_array))) {
      LOG_WARN("fail to add deps to ai column meta json", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    json::ObJsonBuffer j_buf(&allocator);
    if (OB_FAIL(obj.print(j_buf, false))) {
      LOG_WARN("fail to print ai column meta json", K(ret));
    } else {
      json_str = j_buf.string();
    }
  }
  return ret;
}

int parse_ai_column_meta(
    ObIAllocator &allocator,
    const ObString &json_str,
    AIColumnMeta &meta)
{
  int ret = OB_SUCCESS;
  if (json_str.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ai column meta json text is empty", K(ret));
  } else {
    json::ObIJsonBase *base = NULL;
    if (OB_FAIL(json::ObJsonBaseFactory::get_json_base(
            &allocator, json_str,
            ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, base))) {
      LOG_WARN("fail to parse ai column meta json text", K(ret), K(json_str));
    } else if (OB_ISNULL(base) || json::J_OBJECT != base->json_type()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column meta json text is not a json object", K(ret), K(json_str));
    } else {
      json::ObJsonObject *obj = static_cast<json::ObJsonObject *>(base);
      json::ObJsonNode *node = NULL;
      if (OB_ISNULL(node = obj->get_value(ObString("func")))
          || json::J_STRING != node->json_type()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column meta json has no valid func field", K(ret));
      } else if (OB_FAIL(copy_json_string(allocator,
                  static_cast<json::ObJsonString *>(node)->value(), meta.func_))) {
        LOG_WARN("fail to copy func", K(ret));
      } else if (OB_ISNULL(node = obj->get_value(ObString("expr")))
          || json::J_STRING != node->json_type()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column meta json has no valid expr field", K(ret));
      } else if (OB_FAIL(copy_json_string(allocator,
                  static_cast<json::ObJsonString *>(node)->value(), meta.expr_))) {
        LOG_WARN("fail to copy expr", K(ret));
      } else if (OB_ISNULL(node = obj->get_value(ObString("model")))
          || json::J_STRING != node->json_type()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column meta json has no valid model field", K(ret));
      } else if (OB_FAIL(copy_json_string(allocator,
                  static_cast<json::ObJsonString *>(node)->value(), meta.model_id_))) {
        LOG_WARN("fail to copy model", K(ret));
      } else if (OB_ISNULL(node = obj->get_value(ObString("sync_mode")))
          || json::J_STRING != node->json_type()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column meta json has no valid sync_mode field", K(ret));
      } else if (OB_FAIL(copy_json_string(allocator,
                  static_cast<json::ObJsonString *>(node)->value(), meta.sync_mode_))) {
        LOG_WARN("fail to copy sync_mode", K(ret));
      } else if (OB_ISNULL(node = obj->get_value(ObString("max_retries")))
          || json::J_INT != node->json_type()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column meta json has no valid max_retries field", K(ret));
      } else {
        meta.max_retries_ = static_cast<json::ObJsonInt *>(node)->value();
      }
      if (OB_SUCC(ret)) {
        if (OB_ISNULL(node = obj->get_value(ObString("def_version")))
            || json::J_INT != node->json_type()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ai column meta json has no valid def_version field", K(ret));
        } else {
          meta.def_version_ = static_cast<json::ObJsonInt *>(node)->value();
        }
      }
      if (OB_SUCC(ret)) {
        json::ObJsonNode *dep_array_node = obj->get_value(ObString("deps"));
        if (OB_ISNULL(dep_array_node) || json::J_ARRAY != dep_array_node->json_type()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ai column meta json has no valid deps field", K(ret));
        } else {
          json::ObJsonArray *dep_array = static_cast<json::ObJsonArray *>(dep_array_node);
          for (uint64_t i = 0; OB_SUCC(ret) && i < dep_array->element_count(); ++i) {
            json::ObJsonNode *dep_node = dep_array->get_value(i);
            if (OB_ISNULL(dep_node) || json::J_STRING != dep_node->json_type()) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("ai column meta json deps contains invalid element", K(ret), K(i));
            } else {
              ObString dep_name;
              if (OB_FAIL(copy_json_string(allocator,
                          static_cast<json::ObJsonString *>(dep_node)->value(),
                          dep_name))) {
                LOG_WARN("fail to copy dep column name", K(ret), K(i));
              } else if (OB_FAIL(meta.dep_cols_.push_back(dep_name))) {
                LOG_WARN("fail to push back dep column name", K(ret));
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

} // namespace schema
} // namespace share
} // namespace oceanbase
