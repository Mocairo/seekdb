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

#ifndef OB_SPARSE_RETRIEVAL_UTIL_H_
#define OB_SPARSE_RETRIEVAL_UTIL_H_

#include "common/datum/ob_datum.h"
#include "share/datum/ob_datum_funcs.h"

namespace oceanbase
{
namespace storage
{

inline int compare_binary_domain_ids(
    const ObDatum &lhs, const ObDatum &rhs, int &cmp_ret,
    const common::ObDatumAccessContext *access_ctx)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(lhs.is_null() || rhs.is_null() || lhs.is_ext() || rhs.is_ext())) {
    ret = ObDatumFuncs::get_basic_func(common::ObVarcharType, common::CS_TYPE_BINARY)
              ->null_first_cmp_(lhs, rhs, cmp_ret, access_ctx);
  } else {
    // Binary VARCHAR collation compares every byte, including trailing spaces.
    // Document IDs do not require the generic charset lookup and validation.
    const uint32_t common_len = lhs.len_ < rhs.len_ ? lhs.len_ : rhs.len_;
    // Expose the usual 16-byte ID width to the compiler so it can inline the
    // byte comparison. Other lengths retain the same binary collation order.
    const int byte_cmp = common_len == 16 ? MEMCMP(lhs.ptr_, rhs.ptr_, 16)
        : (common_len == 0 ? 0 : MEMCMP(lhs.ptr_, rhs.ptr_, common_len));
    cmp_ret = byte_cmp > 0 ? 1 : (byte_cmp < 0 ? -1
        : (lhs.len_ > rhs.len_ ? 1 : (lhs.len_ < rhs.len_ ? -1 : 0)));
  }
  return ret;
}

inline common::ObDatumCmpFuncType get_domain_id_cmp_func(
    const common::ObObjType type, const common::ObCollationType cs_type)
{
  common::ObDatumCmpFuncType cmp_func = nullptr;
  if (common::ObVarcharType == type && common::CS_TYPE_BINARY == cs_type) {
    cmp_func = compare_binary_domain_ids;
  } else {
    common::ObDatumBasicFuncs *basic_funcs = ObDatumFuncs::get_basic_func(type, cs_type);
    if (nullptr != basic_funcs) {
      cmp_func = basic_funcs->null_first_cmp_;
    }
  }
  return cmp_func;
}

class ObDomainIdCmp
{
public:
  ObDomainIdCmp() : cmp_func_(nullptr) {}
  ~ObDomainIdCmp() {}
  inline int init(const ObObjMeta &obj_meta);
  inline int compare(const ObDatum &lhs, const ObDatum &rhs, int &cmp_ret) const;
  inline void reset() { cmp_func_ = nullptr; }
private:
  common::ObDatumCmpFuncType cmp_func_;
  DISALLOW_COPY_AND_ASSIGN(ObDomainIdCmp);
};

int ObDomainIdCmp::init(const ObObjMeta &obj_meta)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!obj_meta.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN,"invalid argument", K(ret), K(obj_meta));
  } else {
    cmp_func_ = get_domain_id_cmp_func(obj_meta.get_type(), obj_meta.get_collation_type());
    if (OB_ISNULL(cmp_func_)) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN,"failed to get basic functions", K(ret), K(obj_meta));
    }
  }
  return ret;
}

int ObDomainIdCmp::compare(const ObDatum &lhs, const ObDatum &rhs, int &cmp_ret) const
{
  int ret = OB_SUCCESS;
  const bool lmax = lhs.is_max();
  const bool lmin = lhs.is_min();
  const bool rmax = rhs.is_max();
  const bool rmin = rhs.is_min();
  if (OB_UNLIKELY(nullptr == cmp_func_)) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN,"not init", K(ret));
  } else if (lhs.is_ext() || rhs.is_ext()) {
    if (OB_UNLIKELY(!(lmax || lmin || rmax || rmin))) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN,"unexpected ext datum", K(ret), K(lhs), K(rhs));
    } else if (lmax || rmax) {
      cmp_ret = lmax - rmax;
    } else if (lmin || rmin) {
      cmp_ret = -(lmin - rmin);
    } else {
      ret = OB_ERR_UNEXPECTED;
    }
  } else {
    ret = cmp_func_(lhs, rhs, cmp_ret, nullptr);
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase

#endif
