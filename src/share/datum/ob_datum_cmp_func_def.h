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

#ifndef OCEANBASE_OB_DATUM_CMP_FUNC_DEF_H
#define OCEANBASE_OB_DATUM_CMP_FUNC_DEF_H

#include <type_traits>

#include "ob_defined_collations.h"

#include "common/object/ob_obj_type.h"
#include "lib/charset/ob_charset.h"
#include "common/number/ob_number_v2.h"
#include "common/timezone/ob_timezone_info.h"
#include "common/datum/ob_datum.h"
#include "ob_datum_util.h"
#include "common/json_type/ob_json_base.h" // for ObIJsonBase
#include "common/json_type/ob_json_bin.h" // for ObJsonBin
#include "common/wide_integer/ob_wide_integer.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace common
{
struct ObDatumAccessContext;
namespace datum_cmp
{

// We define three core comparison:
//
// 1. ObDatumTypeCmp<ObObjType, ObObjType>: define non string obj type compare,
//    if comparison not deinfed for specified type, use ObDatumTCCmp defined comparison.
// 2. ObDatumTCCmp<ObObjTypeClass, ObObjTypeClass>: define non string obj type class compare
// 3. ObDatumStrCmp<ObCollationType, bool>: define string compare

template <bool V = true>
struct ObDefined
{
  constexpr static bool defined_ = V;
};

// define compare by ObObjTypeClass
template <ObObjTypeClass L_TC, ObObjTypeClass R_TC>
struct ObDatumTCCmp : public ObDefined<false>
{
  inline static int cmp(const ObDatum &, const ObDatum &, int &cmp_ret)
  {
    cmp_ret = 0;
    return OB_SUCCESS;
  }
};

template <ObObjTypeClass TC>
struct ObTCPayloadCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    cmp_ret = ObDatumPayload<TC>::get(l) == ObDatumPayload<TC>::get(r)
              ? 0
              : (ObDatumPayload<TC>::get(l) < ObDatumPayload<TC>::get(r) ? -1 : 1);
    return OB_SUCCESS;
  }
};

// cmp(signed, unsgined)
struct ObSignedUnsignedCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    cmp_ret = l.get_int() < 0
              ? -1
              : (l.get_int() < r.get_uint()
                ? -1
                : (l.get_int() == r.get_uint() ? 0 : 1));
    return OB_SUCCESS;
  }
};

// cmp(unsigned, signed)
struct ObUnsignedSignedCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    int ret = ObSignedUnsignedCmp::cmp(r, l, cmp_ret);
    cmp_ret = -cmp_ret;
    return ret;
  }
};

// specialization for all type class.
template <> struct ObDatumTCCmp<ObIntTC, ObIntTC> : public ObTCPayloadCmp<ObIntTC> {};
template <> struct ObDatumTCCmp<ObUIntTC, ObUIntTC> : public ObTCPayloadCmp<ObUIntTC> {};
template <>
struct ObDatumTCCmp<ObFloatTC, ObFloatTC> : public ObDefined<>
{
  template <typename T>
  inline static int real_value_cmp(T l, T r, int &cmp_ret)
  {
    cmp_ret = 0;
    // Note: For NaN, we can't use C language compare logic.
    // NaN sorts after any numeric value.
    if (isnan(l) || isnan(r)) {
      if (isnan(l) && isnan(r)) {
        cmp_ret = 0;
      } else if (isnan(l)) {
        // l is nan, r is not nan:left always bigger than right
        cmp_ret = 1;
      } else {
        // l is not nan, r is nan, left always less than right
        cmp_ret = -1;
      }
    } else {
      cmp_ret = l == r ? 0 : (l < r ? -1 : 1);
    }
    return OB_SUCCESS;
  }

  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    return real_value_cmp(l.get_float(), r.get_float(), cmp_ret);
  }
};

template <ObScale SCALE>
struct ObFixedDoubleCmp: public ObDefined<>
{
  constexpr static double LOG_10[32] =
  {
    1e000, 1e001, 1e002, 1e003, 1e004, 1e005, 1e006, 1e007,
    1e008, 1e009, 1e010, 1e011, 1e012, 1e013, 1e014, 1e015,
    1e016, 1e017, 1e018, 1e019, 1e020, 1e021, 1e022, 1e023,
    1e024, 1e025, 1e026, 1e027, 1e028, 1e029, 1e030, 1e031
  };
  constexpr static double P = 5 / LOG_10[SCALE + 1];
  inline static int cmp(const ObDatum &l_datum, const ObDatum &r_datum, int &cmp_ret)
  {
    cmp_ret = 0;
    const double l = l_datum.get_double();
    const double r = r_datum.get_double();
    if (isnan(l) || isnan(r)) {
      if (isnan(l) && isnan(r)) {
        cmp_ret = 0;
      } else if (isnan(l)) {
        // l is nan, r is not nan:left always bigger than right
        cmp_ret = 1;
      } else {
        // l is not nan, r is nan, left always less than right
        cmp_ret = -1;
      }
    } else if (l == r || fabs(l - r) < P) {
      cmp_ret = 0;
    } else {
      cmp_ret = (l < r ? -1 : 1);
    }
    return OB_SUCCESS;
  }
};


template <>
struct ObDatumTCCmp<ObDoubleTC, ObDoubleTC> : public ObDatumTCCmp<ObFloatTC, ObFloatTC>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    return ObDatumTCCmp<ObFloatTC, ObFloatTC>::real_value_cmp(l.get_double(), r.get_double(), cmp_ret);
  }
};

template <>
struct ObDatumTCCmp<ObNumberTC, ObNumberTC> : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    cmp_ret = number::ObNumber::compare(l.get_number_desc(), l.get_number_digits(),
                                        r.get_number_desc(), r.get_number_digits());
    return OB_SUCCESS;
  }
};

template<>
struct ObDatumTCCmp<ObDecimalIntTC, ObDecimalIntTC>: public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(wide::compare(l, r, cmp_ret))) {
    }
    return ret;
  }
};

template<ObDecimalIntWideType lw, ObDecimalIntWideType rw>
struct ObDecintCmp: public ObDefined<>
{
  static_assert(lw < DECIMAL_INT_MAX && rw < DECIMAL_INT_MAX, "");
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    decint_cmp_fp cmp =
      wide::ObDecimalIntCmpSet::decint_decint_cmp_set_[(2 << (lw + 1))][(2 << (rw + 1))];
    cmp_ret = cmp(l.get_decimal_int(), r.get_decimal_int());
    return OB_SUCCESS;
  }
};

template <> struct ObDatumTCCmp<ObDateTC, ObDateTC> : public ObTCPayloadCmp<ObDateTC> {};
template <> struct ObDatumTCCmp<ObTimeTC, ObTimeTC> : public ObTCPayloadCmp<ObTimeTC> {};
template <> struct ObDatumTCCmp<ObYearTC, ObYearTC> : public ObTCPayloadCmp<ObYearTC> {};
template <> struct ObDatumTCCmp<ObBitTC, ObBitTC> : public ObTCPayloadCmp<ObBitTC> {};
template <> struct ObDatumTCCmp<ObEnumSetTC, ObEnumSetTC> : public ObTCPayloadCmp<ObEnumSetTC> {};

// different type class compare
template <> struct ObDatumTCCmp<ObIntTC, ObUIntTC> : public ObSignedUnsignedCmp {};
template <> struct ObDatumTCCmp<ObIntTC, ObEnumSetTC> : public ObSignedUnsignedCmp {};
template <> struct ObDatumTCCmp<ObUIntTC, ObIntTC> : public ObUnsignedSignedCmp {};
template <> struct ObDatumTCCmp<ObEnumSetTC, ObIntTC> : public ObUnsignedSignedCmp {};
template <> struct ObDatumTCCmp<ObUIntTC, ObEnumSetTC> : public ObTCPayloadCmp<ObUIntTC> {};
template <> struct ObDatumTCCmp<ObEnumSetTC, ObUIntTC> : public ObTCPayloadCmp<ObUIntTC> {};
template <>
struct ObDatumTCCmp<ObMySQLDateTC, ObMySQLDateTC> : public ObTCPayloadCmp<ObMySQLDateTC> {};
template <>
struct ObDatumTCCmp<ObMySQLDateTimeTC, ObMySQLDateTimeTC> : public ObTCPayloadCmp<ObMySQLDateTimeTC> {};

// special process for extend and null class type.

// extend type vs any type is depend on extend type is min or max
template <ObObjTypeClass TC>
struct ObDatumTCCmp<ObExtendTC, TC> : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &, int &cmp_ret)
  {
    cmp_ret = (ObObj::MIN_OBJECT_VALUE == *l.int_ ? -1 : 1);
    return OB_SUCCESS;
  }
};

template <ObObjTypeClass TC>
struct ObDatumTCCmp<TC, ObExtendTC> : public ObDefined<>
{
  inline static int cmp(const ObDatum &, const ObDatum &r, int &cmp_ret)
  {
    cmp_ret = (ObObj::MIN_OBJECT_VALUE == *r.int_ ? -1 : 1);
    return OB_SUCCESS;
  }
};

template <>
struct ObDatumTCCmp<ObExtendTC, ObExtendTC> : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    cmp_ret = (ObObj::MIN_OBJECT_VALUE == *l.int_ && ObObj::MIN_OBJECT_VALUE == *r.int_)
              || (ObObj::MAX_OBJECT_VALUE == *l.int_ && ObObj::MAX_OBJECT_VALUE == *r.int_)
              ? 0 : (ObObj::MIN_OBJECT_VALUE == *l.int_ ? -1 : 1);
    return OB_SUCCESS;
  }
};

struct ObDummyCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &, const ObDatum &, int &cmp_ret)
  {
    cmp_ret = 0;
    return OB_SUCCESS;
  }
};

// null type compare is never used (out layer guaranteed), but should be defined.
template <ObObjTypeClass TC> struct ObDatumTCCmp<TC, ObNullTC> : public ObDummyCmp {};
template <ObObjTypeClass TC> struct ObDatumTCCmp<ObNullTC, TC> : public ObDummyCmp {};
template <> struct ObDatumTCCmp<ObNullTC, ObExtendTC> : public ObDummyCmp {};
template <> struct ObDatumTCCmp<ObExtendTC, ObNullTC> : public ObDummyCmp {};
template <> struct ObDatumTCCmp<ObNullTC, ObNullTC> : public ObDummyCmp {};


///////////////////////////////////////////////////////////////////////////////
// begin define compare by ObObjType
///////////////////////////////////////////////////////////////////////////////

// define compare by ObObjType
template <ObObjType L_T, ObObjType R_T>
struct ObDatumTypeCmp : public ObDefined<false>
{
  inline static int cmp(const ObDatum &, const ObDatum &, int &cmp_ret)
  {
    cmp_ret = 0;
    return OB_SUCCESS;
  }
};

template <>
struct ObDatumTypeCmp<ObDateTimeType, ObDateTimeType> : public ObTCPayloadCmp<ObDateTimeTC> {};
template <>
struct ObDatumTypeCmp<ObTimestampType, ObTimestampType> : public ObTCPayloadCmp<ObDateTimeTC> {};

struct ObDatumJsonCmpImpl
{
  static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret, const bool is_lob,
                 const ObDatumAccessContext *access_ctx);
};

template <bool HAS_LOB_LOCATOR>
struct ObDatumJsonCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx)
  {
    return ObDatumJsonCmpImpl::cmp(l, r, cmp_ret, HAS_LOB_LOCATOR, access_ctx);
  }
};

struct ObDatumGeoCmpImpl
{
  static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret, const bool is_lob,
                 const ObDatumAccessContext *access_ctx);
};

template <bool HAS_LOB_HEADER>
struct ObDatumGeoCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx)
  {
    return ObDatumGeoCmpImpl::cmp(l, r, cmp_ret, HAS_LOB_HEADER, access_ctx);
  }
};

template <bool HAS_LOB_HEADER>
struct ObDatumUDTCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    UNUSED(l);
    UNUSED(r);
    UNUSED(cmp_ret);
    return OB_ERR_INVALID_TYPE_FOR_OP;
  }
};

struct ObDatumCollectionCmpImpl
{
  static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret, const bool is_lob,
                 const ObDatumAccessContext *access_ctx);
};

template <bool HAS_LOB_HEADER>
struct ObDatumCollectionCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx)
  {
    return ObDatumCollectionCmpImpl::cmp(l, r, cmp_ret, HAS_LOB_HEADER, access_ctx);
  }
};

///////////////////////////////////////////////////////////////////////////////
// begin define string compare functions
///////////////////////////////////////////////////////////////////////////////


struct ObDatumStrCmpImpl
{
  static int OB_INLINE cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                           const ObCollationType cs, const bool with_end_space)
  {
    cmp_ret = ObCharset::strcmpsp(cs, l.ptr_, l.len_, r.ptr_, r.len_, with_end_space);
    cmp_ret = cmp_ret > 0 ? 1 : (cmp_ret < 0 ? -1 : 0);
    return OB_SUCCESS;
  }

  template <typename Word>
  static OB_INLINE bool equal_ends(const char *l, const char *r, const uint32_t len)
  {
    Word l_first, l_last, r_first, r_last;
    MEMCPY(&l_first, l, sizeof(Word));
    MEMCPY(&l_last, l + len - sizeof(Word), sizeof(Word));
    MEMCPY(&r_first, r, sizeof(Word));
    MEMCPY(&r_last, r + len - sizeof(Word), sizeof(Word));
    return l_first == r_first && l_last == r_last;
  }

  static OB_INLINE bool equal_short_string(const char *l, const char *r, const uint32_t len)
  {
    // The overlapping first/last words cover every byte without reading past
    // either payload. Use memcpy so unaligned string data remains supported.
    if (len > 16) {
      return 0 == MEMCMP(l, r, len);
    } else if (len >= 8) {
      return equal_ends<uint64_t>(l, r, len);
    } else if (len >= 4) {
      return equal_ends<uint32_t>(l, r, len);
    } else if (len >= 2) {
      return equal_ends<uint16_t>(l, r, len);
    } else {
      return 0 == len || l[0] == r[0];
    }
  }
};

template <ObCollationType CS_TYPE, bool WITH_END_SPACE>
struct ObDatumStrCmp : public ObDefined<SupportedCollection<CS_TYPE>::defined_>
{
  static OB_INLINE int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    // Binary collation treats every byte as significant, including trailing
    // spaces, for both WITH_END_SPACE modes. Keep exceptional datums on the
    // charset path so its argument handling remains unchanged.
    if (CS_TYPE == CS_TYPE_BINARY
        && !l.is_null() && !r.is_null()
        && ObDatumDesc::NONE == l.flag_ && ObDatumDesc::NONE == r.flag_
        && (0 == l.len_ || nullptr != l.ptr_)
        && (0 == r.len_ || nullptr != r.ptr_)) {
      // Keep the common fixed-width binary key comparison at a constant size
      // so the compiler can compare its two words without a libc dispatch.
      if (16 == l.len_ && 16 == r.len_) {
        const int cmp = MEMCMP(l.ptr_, r.ptr_, 16);
        cmp_ret = cmp > 0 ? 1 : (cmp < 0 ? -1 : 0);
        return OB_SUCCESS;
      }
      const uint32_t common_len = l.len_ < r.len_ ? l.len_ : r.len_;
      const int cmp = 0 == common_len ? 0 : MEMCMP(l.ptr_, r.ptr_, common_len);
      cmp_ret = cmp > 0 ? 1 : (cmp < 0 ? -1
          : (l.len_ > r.len_ ? 1 : (l.len_ < r.len_ ? -1 : 0)));
      return OB_SUCCESS;
    }
    // GENERAL_CI returns at the first unequal character weight. ASCII bytes
    // decode as themselves, so differing first weights need no charset dispatch
    // or suffix decoding. Read the original weights rather than folding bits:
    // punctuation and control bytes do not follow the letter case mapping.
    if (CS_TYPE == CS_TYPE_UTF8MB4_GENERAL_CI
        && !l.is_null() && !r.is_null()
        && ObDatumDesc::NONE == l.flag_ && ObDatumDesc::NONE == r.flag_
        && l.len_ > 0 && r.len_ > 0
        && nullptr != l.ptr_ && nullptr != r.ptr_) {
      const unsigned char l_first = static_cast<unsigned char>(l.ptr_[0]);
      const unsigned char r_first = static_cast<unsigned char>(r.ptr_[0]);
      if (l_first != r_first && (l_first | r_first) < 0x80) {
        const ObUnicaseInfoChar *weights = ob_unicase_default.page[0];
        const uint32_t l_weight = weights[l_first].sort;
        const uint32_t r_weight = weights[r_first].sort;
        if (l_weight != r_weight) {
          cmp_ret = l_weight < r_weight ? -1 : 1;
          return OB_SUCCESS;
        }
      }
    }
    // Repeated short keys need no Unicode decoding when their bytes agree.
    // Only ordinary string payloads reach this comparator; LOB locators use
    // ObDatumTextCmp instead. Keep remaining unequal strings on the collation path.
    if (CS_TYPE == CS_TYPE_UTF8MB4_GENERAL_CI
        && !l.is_null() && !r.is_null()
        && l.len_ == r.len_ && l.len_ > 0 && l.len_ <= 64
        && nullptr != l.ptr_ && nullptr != r.ptr_
        && (l.ptr_ == r.ptr_ || ObDatumStrCmpImpl::equal_short_string(l.ptr_, r.ptr_, l.len_))) {
      cmp_ret = 0;
      return OB_SUCCESS;
    }
    return ObDatumStrCmpImpl::cmp(l, r, cmp_ret, CS_TYPE, WITH_END_SPACE);
  }
};

struct ObDatumTextCmpImpl
{
  static OB_INLINE int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                 const ObCollationType cs, const bool with_end_space,
                 const ObDatumAccessContext *access_ctx)
  {
    int ret = OB_SUCCESS;
    const ObLobCommon& rlob = r.get_lob_data();
    const ObLobCommon& llob = l.get_lob_data();
    if (r.len_ != 0 && !rlob.is_mem_loc_ && rlob.in_row_ &&
        l.len_ != 0 && !llob.is_mem_loc_ && llob.in_row_) {
        cmp_ret = ObCharset::strcmpsp(
            cs, llob.get_inrow_data_ptr(), static_cast<int32_t>(llob.get_byte_size(l.len_)),
            rlob.get_inrow_data_ptr(), static_cast<int32_t>(rlob.get_byte_size(r.len_)), with_end_space);
      cmp_ret = cmp_ret > 0 ? 1 : (cmp_ret < 0 ? -1 : 0);
    } else {
      ret = cmp_out_row(l, r, cmp_ret, cs, with_end_space, access_ctx);
    }
    return ret;
  }

private:
  static int cmp_out_row(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                          const ObCollationType cs, const bool with_end_space,
                          const ObDatumAccessContext *access_ctx);
};

template <ObCollationType CS_TYPE, bool WITH_END_SPACE>
struct ObDatumTextCmp : public ObDefined<SupportedCollection<CS_TYPE>::defined_>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx)
  {
    return ObDatumTextCmpImpl::cmp(
        l, r, cmp_ret, CS_TYPE, WITH_END_SPACE, access_ctx);
  }
};

struct ObDatumTextStringCmpImpl
{
  static OB_INLINE int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                 const ObCollationType cs, const bool with_end_space,
                 const ObDatumAccessContext *access_ctx)
  {
    int ret = OB_SUCCESS;
    const ObLobCommon& llob = l.get_lob_data();
    if (l.len_ != 0 && !llob.is_mem_loc_ && llob.in_row_) {
      cmp_ret = ObCharset::strcmpsp(
          cs, llob.get_inrow_data_ptr(), static_cast<int32_t>(llob.get_byte_size(l.len_)),
          r.ptr_, r.len_, with_end_space);
      cmp_ret = cmp_ret > 0 ? 1 : (cmp_ret < 0 ? -1 : 0);
    } else {
      ret = cmp_out_row(l, r, cmp_ret, cs, with_end_space, access_ctx);
    }
    return ret;
  }

private:
  static int cmp_out_row(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                          const ObCollationType cs, const bool with_end_space,
                          const ObDatumAccessContext *access_ctx);
};

template <ObCollationType CS_TYPE, bool WITH_END_SPACE>
struct ObDatumTextStringCmp : public ObDefined<SupportedCollection<CS_TYPE>::defined_>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx)
  {
    return ObDatumTextStringCmpImpl::cmp(
        l, r, cmp_ret, CS_TYPE, WITH_END_SPACE, access_ctx);
  }
};

struct ObDatumStringTextCmpImpl
{
  static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                 const ObCollationType cs, const bool with_end_space,
                 const ObDatumAccessContext *access_ctx)
  {
    int ret = OB_SUCCESS;
    cmp_ret = 0;
    const ObLobCommon& rlob = r.get_lob_data();
    if (r.len_ != 0 && !rlob.is_mem_loc_ && rlob.in_row_ && rlob.reserve_ == 0) {
      cmp_ret = ObCharset::strcmpsp(
          cs, l.ptr_, l.len_,
          rlob.get_inrow_data_ptr(), static_cast<int32_t>(rlob.get_byte_size(r.len_)), with_end_space);
      cmp_ret = cmp_ret > 0 ? 1 : (cmp_ret < 0 ? -1 : 0);
    } else {
      ret = cmp_out_row(l, r, cmp_ret, cs, with_end_space, access_ctx);
    }
    return ret;
  }
private:
  static int cmp_out_row(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                          const ObCollationType cs, const bool with_end_space,
                          const ObDatumAccessContext *access_ctx);
};

template <ObCollationType CS_TYPE, bool WITH_END_SPACE>
struct ObDatumStringTextCmp : public ObDefined<SupportedCollection<CS_TYPE>::defined_>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx)
  {
    return ObDatumStringTextCmpImpl::cmp(
        l, r, cmp_ret, CS_TYPE, WITH_END_SPACE, access_ctx);
  }
};

} // end namespace datum_cmp
} // end namespace common
} // end namespace oceanbase
#endif // OCEANBASE_OB_DATUM_CMP_FUNC_DEF_H_
