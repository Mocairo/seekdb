/*
 * Copyright (c) 2026 OceanBase.
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

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "query/das/ob_das_id_protocol.h"
#include "storage/retrieval/ob_sparse_daat_iter.h"
#include "storage/retrieval/ob_sparse_retrieval_util.h"

namespace oceanbase
{
namespace storage
{
namespace
{
using namespace common;

TEST(FtsDomainIdCmp, owned_id_copy_matches_datum_deep_copy_at_width_boundaries)
{
  for (const uint32_t length : {0U, 1U, 8U, 15U, 16U, 17U, 39U, 40U, 41U}) {
    for (const int kind : {0, 1, 2}) {
      SCOPED_TRACE(::testing::Message() << length << ", " << kind);
      std::string bytes(length, '\0');
      for (uint32_t i = 0; i < length; ++i) {
        bytes[i] = static_cast<char>(i * 73U);
      }
      ObDatum source;
      source.set_string(bytes.data(), length);
      if (kind == 1) { source.set_null(); }
      if (kind == 2) {
        if (length != 16) { continue; }
        source.set_ext();
      }
      char expected_buffer[40];
      ObDatum expected;
      int64_t pos = 0;
      const int expected_ret = expected.deep_copy(source, expected_buffer, sizeof(expected_buffer), pos);
      sql::ObDocIdExt id;
      ASSERT_EQ(expected_ret, id.from_datum(source));
      const ObDatum &actual = id.get_datum();
      EXPECT_EQ(expected.pack_, actual.pack_);
      if (expected_ret != OB_SUCCESS || source.is_null()) {
        EXPECT_EQ(expected.ptr_, actual.ptr_);
      } else {
        EXPECT_EQ(std::string(expected.ptr_, expected.len_), std::string(actual.ptr_, actual.len_));
        EXPECT_NE(source.ptr_, actual.ptr_);
        bytes.assign(length, '!');
        EXPECT_EQ(std::string(expected.ptr_, expected.len_), std::string(actual.ptr_, actual.len_));
      }
    }
  }
}

TEST(FtsDomainIdCmp, binary_ids_match_generic_collation)
{
  const ObDatumCmpFuncType generic =
      ObDatumFuncs::get_basic_func(ObVarcharType, CS_TYPE_BINARY)->null_first_cmp_;
  const ObDatumCmpFuncType optimized = get_domain_id_cmp_func(ObVarcharType, CS_TYPE_BINARY);
  ASSERT_NE(generic, optimized);
  std::vector<std::string> values = {
      "", "a", "aa", "a ", "a  ", "b", "A", std::string("a\0", 2),
      std::string("a\0b", 3), std::string("a\0a", 3), std::string("\x7f", 1),
      std::string("\x80", 1), std::string("\xff", 1)};
  // Exercise all byte values and equal prefixes at several document-ID lengths.
  for (int length : {1, 8, 16, 32, 64}) {
    for (int start = 0; start < 256; start += 17) {
      std::string value;
      for (int i = 0; i < length; ++i) {
        value.push_back(static_cast<char>((start + i * 29) & 0xff));
      }
      values.push_back(value);
      values.push_back(value + '\0');
    }
  }
  for (size_t i = 0; i < values.size() + 2; ++i) {
    ObDatum lhs;
    if (i < values.size()) {
      lhs.set_string(values[i].data(), values[i].size());
    } else if (i == values.size()) {
      lhs.set_string(nullptr, 0);
    } else {
      lhs.set_null();
    }
    for (size_t j = 0; j < values.size() + 2; ++j) {
      SCOPED_TRACE(::testing::Message() << i << ", " << j);
      ObDatum rhs;
      if (j < values.size()) {
        rhs.set_string(values[j].data(), values[j].size());
      } else if (j == values.size()) {
        rhs.set_string(nullptr, 0);
      } else {
        rhs.set_null();
      }
      int expected = 0;
      int actual = 0;
      ASSERT_EQ(OB_SUCCESS, generic(lhs, rhs, expected, nullptr));
      ASSERT_EQ(OB_SUCCESS, optimized(lhs, rhs, actual, nullptr));
      EXPECT_EQ(expected, actual);
    }
  }
}

TEST(FtsDomainIdCmp, unaligned_binary_ids_compare_every_byte)
{
  const ObDatumCmpFuncType generic =
      ObDatumFuncs::get_basic_func(ObVarcharType, CS_TYPE_BINARY)->null_first_cmp_;
  const ObDatumCmpFuncType optimized = get_domain_id_cmp_func(ObVarcharType, CS_TYPE_BINARY);
  // Equal prefixes reach both halves of an ID. Deliberately offset the data,
  // and include high-bit bytes to detect native-endian or signed comparisons.
  for (int position = 0; position < 16; ++position) {
    for (unsigned char value : {0x00, 0x20, 0x7f, 0x80, 0xff}) {
      std::string left(19, static_cast<char>(0x80));
      std::string right(left);
      right[position + 1] = static_cast<char>(value);
      ObDatum lhs, rhs;
      lhs.set_string(left.data() + 1, 16);
      rhs.set_string(right.data() + 1, 16);
      int expected = 0;
      int actual = 0;
      ASSERT_EQ(OB_SUCCESS, generic(lhs, rhs, expected, nullptr));
      ASSERT_EQ(OB_SUCCESS, optimized(lhs, rhs, actual, nullptr));
      EXPECT_EQ(expected, actual) << position << ", " << static_cast<int>(value);
      ASSERT_EQ(OB_SUCCESS, optimized(rhs, lhs, actual, nullptr));
      EXPECT_EQ(-expected, actual);
    }
  }
}

TEST(FtsDomainIdCmp, other_types_and_collations_keep_generic_comparison)
{
  for (ObObjType type : {ObUInt64Type, ObIntType, ObCharType}) {
    EXPECT_EQ(ObDatumFuncs::get_basic_func(type, CS_TYPE_BINARY)->null_first_cmp_,
              get_domain_id_cmp_func(type, CS_TYPE_BINARY));
  }
  for (ObCollationType collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI}) {
    EXPECT_EQ(ObDatumFuncs::get_basic_func(ObVarcharType, collation)->null_first_cmp_,
              get_domain_id_cmp_func(ObVarcharType, collation));
  }
}

TEST(FtsDomainIdCmp, block_boundaries_keep_min_max_and_null_ordering)
{
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_BINARY);
  ObDomainIdCmp cmp;
  ObDatum data;
  data.set_string("doc", 3);
  int result = 0;
  EXPECT_EQ(OB_NOT_INIT, cmp.compare(data, data, result));
  ASSERT_EQ(OB_SUCCESS, cmp.init(meta));

  ObObj minimum, maximum;
  minimum.set_min_value();
  maximum.set_max_value();
  ObDatum min_datum, max_datum, null_datum;
  min_datum.ptr_ = reinterpret_cast<const char *>(&minimum);
  min_datum.set_ext();
  max_datum.ptr_ = reinterpret_cast<const char *>(&maximum);
  max_datum.set_ext();
  null_datum.set_null();
  const ObDatum *ordered[] = {&min_datum, &null_datum, &data, &max_datum};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      SCOPED_TRACE(::testing::Message() << i << ", " << j);
      ASSERT_EQ(OB_SUCCESS, cmp.compare(*ordered[i], *ordered[j], result));
      EXPECT_EQ(i < j ? -1 : (i > j ? 1 : 0), result);
    }
  }
  cmp.reset();
  EXPECT_EQ(OB_NOT_INIT, cmp.compare(data, data, result));
}

TEST(FtsDomainIdCmp, merge_comparison_observes_live_slots_and_reinitialization)
{
  ObArenaAllocator allocator;
  ObFixedArray<const ObDatum *, ObIAllocator> first_ids(&allocator);
  ASSERT_EQ(OB_SUCCESS, first_ids.init(2));
  ASSERT_EQ(OB_SUCCESS, first_ids.prepare_allocate(2));
  ObDatum alpha, beta, omega, null_datum;
  alpha.set_string("a", 1);
  beta.set_string("b", 1);
  omega.set_string("z", 1);
  null_datum.set_null();
  first_ids.at(0) = &alpha;
  first_ids.at(1) = &beta;
  sql::ObDatumMeta meta;
  meta.type_ = ObVarcharType;
  meta.cs_type_ = CS_TYPE_BINARY;
  ObSRMergeCmp cmp;
  ObSRMergeItem lhs, rhs;
  lhs.iter_idx_ = 0;
  rhs.iter_idx_ = 1;
  int64_t result = 0;
  EXPECT_EQ(OB_NOT_INIT, cmp.cmp(lhs, rhs, result));
  ASSERT_EQ(OB_SUCCESS, cmp.init(meta, &first_ids));
  ASSERT_EQ(OB_SUCCESS, cmp.cmp(lhs, rhs, result));
  EXPECT_EQ(-1, result);

  // Heap refills replace slots and can update the datum behind an existing slot.
  first_ids.at(0) = &omega;
  ASSERT_EQ(OB_SUCCESS, cmp.cmp(lhs, rhs, result));
  EXPECT_EQ(1, result);
  beta.set_string("z", 1);
  ASSERT_EQ(OB_SUCCESS, cmp.cmp(lhs, rhs, result));
  EXPECT_EQ(0, result);
  first_ids.at(1) = &null_datum;
  ASSERT_EQ(OB_SUCCESS, cmp.cmp(lhs, rhs, result));
  EXPECT_EQ(1, result);

  ObFixedArray<const ObDatum *, ObIAllocator> second_ids(&allocator);
  ASSERT_EQ(OB_SUCCESS, second_ids.init(2));
  ASSERT_EQ(OB_SUCCESS, second_ids.prepare_allocate(2));
  int64_t negative_value = 0;
  int64_t positive_value = 0;
  ObDatum negative, positive;
  negative.ptr_ = reinterpret_cast<const char *>(&negative_value);
  positive.ptr_ = reinterpret_cast<const char *>(&positive_value);
  negative.set_int(-7);
  positive.set_int(42);
  second_ids.at(0) = &negative;
  second_ids.at(1) = &positive;
  first_ids.reset();
  meta.type_ = ObIntType;
  ASSERT_EQ(OB_SUCCESS, cmp.init(meta, &second_ids));
  ASSERT_EQ(OB_SUCCESS, cmp.cmp(lhs, rhs, result));
  EXPECT_EQ(-1, result);
  second_ids.at(0) = &positive;
  second_ids.at(1) = &negative;
  ASSERT_EQ(OB_SUCCESS, cmp.cmp(lhs, rhs, result));
  EXPECT_EQ(1, result);
}

} // namespace
} // namespace storage
} // namespace oceanbase
