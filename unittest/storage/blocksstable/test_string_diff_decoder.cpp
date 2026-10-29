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
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "lib/allocator/page_arena.h"
#include "lib/utility/ob_target_specific.h"
#include "storage/blocksstable/encoding/ob_string_diff_decoder.h"
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__GNUC__) && defined(__x86_64__)
#include <cpuid.h>
#endif

namespace oceanbase
{
namespace blocksstable
{
namespace
{
using namespace common;

// Both encoded input and decoder allocations end immediately before an
// inaccessible page. In particular, payloads shorter than 16 have no readable
// padding after the final physical row.
class StringDiffGuardedBuffer
{
public:
  explicit StringDiffGuardedBuffer(const int64_t size) : data_(nullptr), size_(size)
  {
#if defined(__linux__)
    const int64_t page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) { return; }
    mapping_size_ = ((size + page_size - 1) / page_size + 2) * page_size;
    mapping_ = mmap(nullptr, mapping_size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping_ != MAP_FAILED) {
      char *writable = static_cast<char *>(mapping_) + page_size;
      if (mprotect(writable, mapping_size_ - 2 * page_size, PROT_READ | PROT_WRITE) == 0) {
        data_ = static_cast<char *>(mapping_) + mapping_size_ - page_size - size;
      }
    }
#else
    storage_.reset(new char[size]);
    data_ = storage_.get();
#endif
    if (data_ != nullptr) { MEMSET(data_, 0, size_); }
  }
  ~StringDiffGuardedBuffer()
  {
#if defined(__linux__)
    if (mapping_ != MAP_FAILED) { munmap(mapping_, mapping_size_); }
#endif
  }
  char *data() const { return data_; }
  int64_t size() const { return size_; }
private:
  char *data_;
  int64_t size_;
#if defined(__linux__)
  void *mapping_ = MAP_FAILED;
  int64_t mapping_size_ = 0;
#else
  std::unique_ptr<char[]> storage_;
#endif
};

class StringDiffExactAllocator : public ObIAllocator
{
public:
  void *alloc(const int64_t size) override
  {
    ++calls_;
    last_size_ = size;
    if (fail_) { return nullptr; }
    buffers_.emplace_back(new StringDiffGuardedBuffer(size));
    return buffers_.back()->data();
  }
  void *alloc(const int64_t size, const ObMemAttr &) override { return alloc(size); }
  void free(void *) override {}
  bool fail_ = false;
  int64_t calls_ = 0;
  int64_t last_size_ = 0;
private:
  std::vector<std::unique_ptr<StringDiffGuardedBuffer>> buffers_;
};

class StringDiffFeatureScope
{
public:
  explicit StringDiffFeatureScope(const bool detect = true) : saved_(arches)
  {
    if (detect) { init_arches(); } else { arches = 0; }
  }
  ~StringDiffFeatureScope() { arches = saved_; }
  static bool has_fast_decoder()
  {
#if defined(__GNUC__) && defined(__x86_64__)
    unsigned int a = 0, b = 0, c = 0, d = 0;
    return is_arch_supported(ObTargetArch::SSE42)
        && __get_cpuid(1, &a, &b, &c, &d) && (c & bit_SSSE3) != 0;
#else
    return false;
#endif
  }
private:
  uint32_t saved_;
};

class StringDiffFailingRowIndex : public ObIRowIndex
{
public:
  explicit StringDiffFailingRowIndex(const ObIRowIndex &index) : index_(index) {}
  int get(const int64_t row, const char *&data, int64_t &len) const override
  {
    return ++calls_ == 2 ? OB_EAGAIN : index_.get(row, data, len);
  }
  int batch_get(const int32_t *, const int64_t, const bool, const char **,
      ObDatum *) const override { return OB_NOT_SUPPORTED; }
  int batch_get(const int32_t *, const int64_t, const bool, const char **,
      uint32_t *) const override { return OB_NOT_SUPPORTED; }
  mutable int64_t calls_ = 0;
private:
  const ObIRowIndex &index_;
};

class StringDiffCase
{
public:
  int init(const std::vector<bool> &diff, const int64_t row_count,
      const bool fixed = true, const bool hex = false,
      const std::vector<int> &ext = {}, const int64_t seed = 0)
  {
    const int64_t string_size = diff.size();
    const int64_t diff_count = std::count(diff.begin(), diff.end(), true);
    const int64_t payload_size = hex ? (diff_count + 1) / 2 : diff_count;
    const int64_t ext_bits = ext.empty() ? 0 :
        (*std::max_element(ext.begin(), ext.end()) > 1 ? 2 : 1);
    const int64_t fixed_ext_size = fixed ? (row_count * ext_bits + 7) / 8 : 0;
    const int64_t row_ext_size = !fixed && ext_bits != 0 ? 1 : 0;
    std::vector<ObStringDiffHeader::DiffDesc> descs;
    for (int64_t i = 0; i < string_size;) {
      int64_t end = i + 1;
      while (end < string_size && diff[end] == diff[i] && end - i < 127) { ++end; }
      ObStringDiffHeader::DiffDesc desc;
      desc.diff_ = diff[i];
      desc.count_ = end - i;
      descs.push_back(desc);
      i = end;
    }
    const int64_t meta_size = sizeof(ObStringDiffHeader) + descs.size()
        + (hex ? 16 : 0) + string_size - diff_count;
    const int64_t data_size = fixed_ext_size + row_count * (payload_size + row_ext_size);
    bytes_.reset(new StringDiffGuardedBuffer(meta_size + data_size));
    if (bytes_->data() == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
    auto *header = new (bytes_->data()) ObStringDiffHeader();
    header->string_size_ = string_size;
    header->diff_desc_cnt_ = descs.size();
    header->length_ = payload_size;
    header->offset_ = row_ext_size;
    header->hex_char_array_size_ = hex ? 16 : 0;
    MEMCPY(header->diff_descs_, descs.data(), descs.size());
    if (hex) {
      for (int64_t i = 0; i < 16; ++i) { header->hex_char_array()[i] = i * 17; }
    }
    int64_t common_pos = 0;
    for (int64_t i = 0; i < string_size; ++i) {
      if (!diff[i]) { header->common_data()[common_pos++] = static_cast<char>(i * 73 + seed * 19); }
    }
    char *rows = bytes_->data() + meta_size;
    values_.resize(row_count);
    for (int64_t row = 0; row < row_count; ++row) {
      char *payload = rows + fixed_ext_size + row * (payload_size + row_ext_size);
      if (!ext.empty()) {
        const int64_t bit = fixed ? row * ext_bits : 0;
        unsigned char *ext_data = reinterpret_cast<unsigned char *>(fixed ? rows : payload);
        ext_data[bit / 8] |= static_cast<unsigned char>(ext[row] << (bit % 8));
      }
      payload += row_ext_size;
      int64_t packed_pos = 0;
      values_[row].resize(string_size);
      for (int64_t pos = 0; pos < string_size; ++pos) {
        if (!diff[pos]) {
          values_[row][pos] = static_cast<char>(pos * 73 + seed * 19);
        } else if (hex) {
          const unsigned char index = (row + pos + seed) % 16;
          values_[row][pos] = static_cast<char>(index * 17);
          payload[packed_pos / 2] |= static_cast<char>(index << ((1 - packed_pos % 2) * 4));
          ++packed_pos;
        } else {
          const char value = static_cast<char>(row * 37 + pos * 73 + seed * 19);
          values_[row][pos] = value;
          payload[packed_pos++] = value;
        }
      }
    }
    micro_.row_count_ = row_count;
    micro_.extend_value_bit_ = ext_bits;
    micro_.var_column_count_ = fixed ? 0 : 1;
    col_.type_ = ObColumnHeader::STRING_DIFF;
    col_.obj_type_ = ObVarcharType;
    col_.length_ = meta_size;
    if (fixed) { col_.set_fix_lenght_attr(); }
    if (ext_bits != 0) { col_.set_has_extend_value_attr(); }
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(CS_TYPE_BINARY);
    ctx_.fill(meta, &micro_, &col_, &allocator_, 0);
    int ret = index_.init(rows, fixed ? 0 : data_size, row_count);
    if (ret == OB_SUCCESS) { ret = decoder_.init(micro_, col_, bytes_->data()); }
    return ret;
  }

  void compare(const std::vector<int32_t> &ids, const bool scalar = true)
  {
    SCOPED_TRACE(::testing::Message() << values_[0].size() << ", " << ids.size());
    std::vector<ObDatum> actual(ids.size() + 2);
    std::vector<ObDatum> expected(ids.size());
    const char *sentinel = "sentinel";
    actual.front().set_string(sentinel, 8);
    actual.back().set_string(sentinel, 8);
    const ObDatum first = actual.front();
    const ObDatum last = actual.back();
    const char *unused = nullptr;
    ASSERT_EQ(OB_SUCCESS, decoder_.batch_decode(ctx_, &index_, ids.data(),
        &unused, ids.size(), actual.data() + 1));
    EXPECT_EQ(first.pack_, actual.front().pack_);
    EXPECT_EQ(first.ptr_, actual.front().ptr_);
    EXPECT_EQ(last.pack_, actual.back().pack_);
    EXPECT_EQ(last.ptr_, actual.back().ptr_);
    ObArenaAllocator golden_allocator;
    ObColumnDecoderCtx golden_ctx = ctx_;
    golden_ctx.allocator_ = &golden_allocator;
    {
      StringDiffFeatureScope disabled(false);
      ASSERT_EQ(OB_SUCCESS, decoder_.batch_decode(golden_ctx, &index_, ids.data(),
          &unused, ids.size(), expected.data()));
    }
    for (int64_t i = 0; i < ids.size(); ++i) {
      const ObDatum &result = actual[i + 1];
      EXPECT_EQ(expected[i].pack_, result.pack_);
      if (!result.is_null()) {
        EXPECT_EQ(std::string(expected[i].ptr_, expected[i].len_),
            std::string(result.ptr_, result.len_));
        EXPECT_EQ(values_[ids[i]], std::string(result.ptr_, result.len_));
        if (i != 0) { EXPECT_NE(actual[i].ptr_, result.ptr_); }
      }
      if (scalar) {
        const char *row_data = nullptr;
        int64_t row_len = 0;
        ASSERT_EQ(OB_SUCCESS, index_.get(ids[i], row_data, row_len));
        ObBitStream bits(reinterpret_cast<unsigned char *>(const_cast<char *>(row_data)), row_len);
        ObDatum datum;
        ASSERT_EQ(OB_SUCCESS, decoder_.decode(golden_ctx, datum, ids[i], bits, row_data, row_len));
        EXPECT_EQ(datum.pack_, result.pack_);
        if (!datum.is_null()) {
          EXPECT_EQ(std::string(datum.ptr_, datum.len_), std::string(result.ptr_, result.len_));
        }
      }
    }
  }

  ObMicroBlockHeader micro_;
  ObColumnHeader col_;
  ObColumnDecoderCtx ctx_;
  ObStringDiffDecoder decoder_;
  ObFixRowIndex index_;
  StringDiffExactAllocator allocator_;
  std::unique_ptr<StringDiffGuardedBuffer> bytes_;
  std::vector<std::string> values_;
};

TEST(StringDiffDecoder, every_short_width_and_packed_length_matches_scalar_and_original_batch)
{
  StringDiffFeatureScope features;
  for (int64_t width = 1; width <= 16; ++width) {
    for (int64_t packed = 1; packed <= width; ++packed) {
      for (const int64_t layout : {0, 1, 2}) {
        std::vector<bool> diff(width, false);
        for (int64_t i = 0; i < packed; ++i) { diff[(i + layout * (width / 2 + 1)) % width] = true; }
        StringDiffCase test;
        ASSERT_EQ(OB_SUCCESS, test.init(diff, 17, true, false, {}, layout));
        test.compare({16, 0, 8, 16, 1, 15, 7});
        EXPECT_EQ(1, test.allocator_.calls_);
        EXPECT_EQ((StringDiffFeatureScope::has_fast_decoder() ? 16 : 128) * 7,
            test.allocator_.last_size_);
      }
    }
  }
}

TEST(StringDiffDecoder, alternating_fragments_and_batch_boundaries_preserve_every_byte)
{
  StringDiffFeatureScope features;
  for (const int64_t count : {1, 2, 63, 64, 65, 129}) {
    for (const int64_t parity : {0, 1}) {
      std::vector<bool> diff(16, false);
      for (int64_t i = 0; i < 16; ++i) { diff[i] = i % 2 == parity; }
      StringDiffCase test;
      ASSERT_EQ(OB_SUCCESS, test.init(diff, 129, true, false, {}, parity));
      std::vector<int32_t> ids(count);
      for (int64_t i = 0; i < count; ++i) { ids[i] = 128 - i; }
      test.compare(ids);
    }
  }
}

TEST(StringDiffDecoder, all_binary_byte_values_survive_reconstruction)
{
  StringDiffFeatureScope features;
  StringDiffCase test;
  ASSERT_EQ(OB_SUCCESS, test.init(std::vector<bool>(16, true), 256));
  std::vector<int32_t> ids(256);
  for (int64_t i = 0; i < 256; ++i) { ids[i] = 255 - i; }
  test.compare(ids);
}

TEST(StringDiffDecoder, unsupported_cpu_uses_original_allocation_and_decoder)
{
  StringDiffFeatureScope disabled(false);
  StringDiffCase test;
  ASSERT_EQ(OB_SUCCESS, test.init({false, true, false, true}, 3));
  test.compare({2, 0, 2});
  EXPECT_EQ(128 * 3, test.allocator_.last_size_);
}

TEST(StringDiffDecoder, long_variable_and_hex_formats_keep_original_batch_semantics)
{
  StringDiffFeatureScope features;
  for (const int64_t width : {1, 16, 17, 128, 129}) {
    for (const bool fixed : {false, true}) {
      for (const bool hex : {false, true}) {
        std::vector<bool> diff(width, true);
        if (width > 1) { diff[0] = false; }
        StringDiffCase test;
        ASSERT_EQ(OB_SUCCESS, test.init(diff, 17, fixed, hex));
        test.compare({16, 0, 8, 16, 1});
        if (!fixed || hex || width > 16) {
          EXPECT_EQ(std::max<int64_t>(128, width) * 5, test.allocator_.last_size_);
        }
      }
    }
  }
}

TEST(StringDiffDecoder, null_and_nope_columns_stay_on_original_batch_path)
{
  StringDiffFeatureScope features;
  for (const bool fixed : {false, true}) {
    for (const bool all_null : {false, true}) {
      std::vector<int> ext(17, 1);
      if (!all_null) { for (int64_t i = 1; i < 16; ++i) { ext[i] = i % 3 == 0 ? 1 : 0; } }
      StringDiffCase test;
      ASSERT_EQ(OB_SUCCESS, test.init({false, true, false, true}, 17, fixed, false, ext));
      test.compare({16, 0, 8, 4, 2, 1, 16});
      EXPECT_EQ(128 * 7, test.allocator_.last_size_);
    }
    // The vector scan contract excludes NOP. Its existing decoder treats all
    // nonzero extension values as NULL; compare that exact fallback behavior.
    std::vector<int> ext(17, 0);
    ext[0] = STORED_NULL;
    ext[8] = STORED_NOPE;
    ext[16] = STORED_NOPE;
    StringDiffCase test;
    ASSERT_EQ(OB_SUCCESS, test.init({false, true, false, true}, 17, fixed, false, ext));
    test.compare({16, 0, 8, 4, 2, 1, 16}, false);
    EXPECT_EQ(128 * 7, test.allocator_.last_size_);
  }
}

TEST(StringDiffDecoder, allocation_failure_is_returned_once_without_touching_datums)
{
  StringDiffFeatureScope features;
  for (const bool disable : {false, true}) {
    StringDiffFeatureScope restore;
    if (disable) { arches = 0; }
    StringDiffCase test;
    ASSERT_EQ(OB_SUCCESS, test.init({false, true, false, true}, 3));
    test.allocator_.fail_ = true;
    ObDatum datums[3];
    for (auto &datum : datums) { datum.set_string("unchanged", 9); }
    const int32_t ids[] = {2, 0, 2};
    const char *unused = nullptr;
    EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, test.decoder_.batch_decode(
        test.ctx_, &test.index_, ids, &unused, 3, datums));
    EXPECT_EQ(1, test.allocator_.calls_);
    for (const auto &datum : datums) {
      EXPECT_EQ(9U, static_cast<uint32_t>(datum.len_));
      EXPECT_EQ(std::string("unchanged"), std::string(datum.ptr_, datum.len_));
    }
  }
}

TEST(StringDiffDecoder, variable_row_error_stops_before_later_datums)
{
  StringDiffFeatureScope features;
  for (const bool disable : {false, true}) {
    StringDiffFeatureScope restore;
    if (disable) { arches = 0; }
    StringDiffCase test;
    ASSERT_EQ(OB_SUCCESS, test.init({false, true, false, true}, 3, false));
    StringDiffFailingRowIndex index(test.index_);
    ObDatum datums[3];
    for (auto &datum : datums) { datum.set_string("unchanged", 9); }
    const int32_t ids[] = {2, 0, 1};
    const char *unused = nullptr;
    EXPECT_EQ(OB_EAGAIN, test.decoder_.batch_decode(
        test.ctx_, &index, ids, &unused, 3, datums));
    EXPECT_EQ(2, index.calls_);
    EXPECT_EQ(test.values_[2], std::string(datums[0].ptr_, datums[0].len_));
    for (const int64_t i : {1, 2}) {
      EXPECT_EQ(std::string("unchanged"), std::string(datums[i].ptr_, datums[i].len_));
    }
  }
}

TEST(StringDiffDecoder, nullable_allocation_error_preserves_original_partial_state)
{
  StringDiffFeatureScope features;
  for (const bool fixed : {false, true}) {
    for (const int ext_value : {STORED_NULL, STORED_NOPE}) {
      std::vector<ObDatum> expected;
      for (const bool disable : {true, false}) {
        StringDiffFeatureScope restore;
        if (disable) { arches = 0; }
        StringDiffCase test;
        ASSERT_EQ(OB_SUCCESS, test.init({false, true, false, true}, 3, fixed,
            false, {ext_value, 0, ext_value}));
        test.allocator_.fail_ = true;
        std::vector<ObDatum> actual(3);
        for (auto &datum : actual) { datum.set_string("unchanged", 9); }
        const int32_t ids[] = {2, 1, 0};
        const char *unused = nullptr;
        EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, test.decoder_.batch_decode(
            test.ctx_, &test.index_, ids, &unused, 3, actual.data()));
        EXPECT_EQ(1, test.allocator_.calls_);
        if (disable) {
          expected = actual;
        } else {
          for (int64_t i = 0; i < 3; ++i) {
            EXPECT_EQ(expected[i].pack_, actual[i].pack_);
            EXPECT_EQ(expected[i].ptr_, actual[i].ptr_);
          }
        }
      }
    }
  }
}

TEST(StringDiffDecoder, pointer_update_and_reuse_rebuild_the_plan_from_current_metadata)
{
  StringDiffFeatureScope features;
  StringDiffCase test;
  ASSERT_EQ(OB_SUCCESS, test.init({false, true, false, true, true, false}, 17));
  test.compare({16, 0, 8});
  std::unique_ptr<StringDiffGuardedBuffer> moved(new StringDiffGuardedBuffer(test.bytes_->size()));
  ASSERT_NE(nullptr, moved->data());
  MEMCPY(moved->data(), test.bytes_->data(), test.bytes_->size());
  ASSERT_EQ(OB_SUCCESS, test.decoder_.update_pointer(test.bytes_->data(), moved->data()));
  test.bytes_.swap(moved);
  moved.reset();
  ASSERT_EQ(OB_SUCCESS, test.index_.init(test.bytes_->data() + test.col_.length_, 0, 17));
  test.compare({0, 16, 8, 0});
  test.decoder_.reuse();
  ASSERT_EQ(OB_SUCCESS, test.decoder_.init(test.micro_, test.col_, test.bytes_->data()));
  test.compare({8, 16, 0, 8});
}

TEST(StringDiffDecoder, returned_bytes_are_owned_by_the_decoder_allocator)
{
  StringDiffFeatureScope features;
  StringDiffCase test;
  ASSERT_EQ(OB_SUCCESS, test.init(std::vector<bool>(16, true), 3));
  const int32_t ids[] = {2, 0, 2};
  const char *unused = nullptr;
  ObDatum datums[3];
  ASSERT_EQ(OB_SUCCESS, test.decoder_.batch_decode(
      test.ctx_, &test.index_, ids, &unused, 3, datums));
  test.bytes_.reset();
  for (int64_t i = 0; i < 3; ++i) {
    EXPECT_EQ(test.values_[ids[i]], std::string(datums[i].ptr_, datums[i].len_));
  }
}

TEST(StringDiffDecoder, uninitialized_decoder_returns_original_error_without_allocation)
{
  StringDiffFeatureScope features;
  StringDiffCase test;
  const int32_t row = 0;
  const char *unused = nullptr;
  ObDatum datum;
  datum.set_null();
  EXPECT_EQ(OB_NOT_INIT, test.decoder_.batch_decode(
      test.ctx_, &test.index_, &row, &unused, 1, &datum));
  EXPECT_TRUE(datum.is_null());
  EXPECT_EQ(0, test.allocator_.calls_);
}
} // namespace
} // namespace blocksstable
} // namespace oceanbase
