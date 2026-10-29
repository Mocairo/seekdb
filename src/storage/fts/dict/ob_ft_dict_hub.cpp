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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
class ObFTDictHub::BuiltinDictView
{
public:
  explicit BuiltinDictView(const ObFTDictDesc &desc)
      : allocator_(lib::ObMemAttr("FTDictView")), container_(allocator_),
        dict_(allocator_, &container_, desc)
  {}

  ObArenaAllocator allocator_;
  ObFTCacheRangeContainer container_;
  ObFTRangeDict dict_;
};

ObFTDictHub::ObFTDictHub()
    : is_inited_(false), dict_map_(), rw_dict_lock_(), builtin_dict_lock_(),
      builtin_dicts_{nullptr, nullptr, nullptr}
{}

int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
    dict_map_.destroy();
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  // All parsers must stop before the hub, and cache handles must be released
  // before ObDictCache / ObKVGlobalCache are destroyed.
  lib::ObMutexGuard guard(builtin_dict_lock_);
  for (int64_t i = 0; i < BUILTIN_DICT_COUNT; ++i) {
    BuiltinDictView *view = builtin_dicts_[i].exchange(nullptr, std::memory_order_acq_rel);
    if (nullptr != view) {
      view->~BuiltinDictView();
      ob_free(view);
    }
  }
  dict_map_.destroy();
  rw_dict_lock_.destroy();
  is_inited_ = false;
  return ret;
}

int ObFTDictHub::get_builtin_dict(const ObFTDictType type, const ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  dict = nullptr;
  const int64_t index = static_cast<int64_t>(type) - 1;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (index < 0 || index >= BUILTIN_DICT_COUNT) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    BuiltinDictView *view = builtin_dicts_[index].load(std::memory_order_acquire);
    if (nullptr == view) {
      lib::ObMutexGuard guard(builtin_dict_lock_);
      view = builtin_dicts_[index].load(std::memory_order_relaxed);
      if (nullptr == view) {
        static const char *names[BUILTIN_DICT_COUNT] = {"main_dict", "quan_dict", "stopword"};
        const ObFTDictDesc desc(names[index], type, CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN);
        void *buf = ob_malloc(sizeof(BuiltinDictView), lib::ObMemAttr("FTDictView"));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          view = new (buf) BuiltinDictView(desc);
          if (OB_FAIL(load_cache(desc, view->container_)) && OB_ENTRY_NOT_EXIST == ret) {
            ret = build_cache(desc, view->container_);
          }
          if (OB_SUCC(ret) && OB_FAIL(view->dict_.init())) {
          }
          if (OB_SUCC(ret)) {
            builtin_dicts_[index].store(view, std::memory_order_release);
          } else {
            view->~BuiltinDictView();
            ob_free(view);
            view = nullptr;
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      dict = &view->dict_;
    }
  }
  return ret;
}
int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
