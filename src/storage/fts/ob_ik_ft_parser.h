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

#ifndef _OCEANBASE_STORAGE_FTS_OB_IK_FT_PARSER_H_
#define _OCEANBASE_STORAGE_FTS_OB_IK_FT_PARSER_H_

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/lock/ob_spin_lock.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/ik/ob_ik_processor.h"
#include "data_plane/fts/ob_fts_parser.h"

#include <atomic>
#include <cstdint>
namespace oceanbase
{
namespace storage
{
class ObFTDictHub;

class ObIKFTParser final : public ObITokenIterator
{
public:
  // dicts are owned by ObIKFTParserDesc and shared among all parsers.
  ObIKFTParser(ObIAllocator &allocator, ObIFTDict *dict_main, ObIFTDict *dict_quan, ObIFTDict *dict_stop)
      : allocator_(allocator),
        is_inited_(false),
        coll_type_(ObCollationType::CS_TYPE_INVALID),
        ctx_(nullptr),
        segmenters_(allocator_),
        dict_main_(dict_main),
        dict_quan_(dict_quan),
        dict_stop_(dict_stop)
  {
  }

  virtual ~ObIKFTParser() { reset(); }

  int init(const ObFTParserParam &param);

  int get_next_token(const char *&word,
                     int64_t &word_len,
                     int64_t &char_cnt,
                     int64_t &word_freq) override;

  VIRTUAL_TO_STRING_KV(K(is_inited_));

private:
  int produce();

  int process_next_batch();

  int process_one_char(TokenizeContext &ctx,
                       const char *ch,
                       const uint8_t char_len,
                       const ObFTCharUtil::CharType type);

private:
  int init_ctx(const ObFTParserParam &param);

  int init_segmenter(const ObFTParserParam &param);

  void reset();

private:
  static constexpr int SEGMENT_LIMIT = 1000;
  ObIAllocator &allocator_;
  bool is_inited_;

  ObCollationType coll_type_;
  TokenizeContext *ctx_;
  ObList<ObIIKProcessor *, ObIAllocator> segmenters_;

  // dicts are owned by ObIKFTParserDesc, read-only here.
  ObIFTDict *dict_main_;
  ObIFTDict *dict_quan_;
  ObIFTDict *dict_stop_;

  DISABLE_COPY_ASSIGN(ObIKFTParser);
};

class ObIKFTParserDesc final : public ObIFTParserDesc
{
public:
  ObIKFTParserDesc();
  virtual ~ObIKFTParserDesc();
  virtual int segment(ObFTParserParam *param, ObITokenIterator *&iter) const override;
  virtual void free_token_iter(ObFTParserParam *param, ObITokenIterator *&iter) const override;
  virtual int get_add_word_flag(ObAddWordFlag &flag) const override;

private:
  // Build the three built-in IK dicts once and share them among all parsers.
  // The dicts are immutable (built-in dicts, should_read_newest_table is always false),
  // so caching them at desc level keeps the tokenize semantics unchanged.
  int get_or_build_dicts() const;

  int init_single_dict(ObFTDictHub *hub, const ObFTDictDesc &desc,
                       ObFTCacheRangeContainer *&container) const;

  int build_dict_from_cache(const ObFTDictDesc &desc,
                            ObFTCacheRangeContainer *&container,
                            ObIFTDict *&dict) const;

  void destroy_dicts() const;

private:
  mutable common::ObSpinLock dict_lock_;
  mutable std::atomic<bool> dicts_ready_;
  mutable lib::ObMemAttr dict_alloc_attr_;
  mutable ObArenaAllocator dict_alloc_;
  mutable ObFTCacheRangeContainer *cache_main_;
  mutable ObFTCacheRangeContainer *cache_quan_;
  mutable ObFTCacheRangeContainer *cache_stop_;
  mutable ObIFTDict *dict_main_;
  mutable ObIFTDict *dict_quan_;
  mutable ObIFTDict *dict_stop_;
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_OB_IK_FT_PARSER_H_
