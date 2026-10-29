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
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "storage/fts/dict/ob_ft_dat_dict.h"

namespace oceanbase
{
namespace storage
{
namespace
{
using namespace common;

// Independently build the original packed map, including the original modulo
// probe step. The optimized reader must also consume this unmodified layout.
class LegacyDatCodeMap
{
public:
  explicit LegacyDatCodeMap(const size_t word_count)
      : size_(ObArrayHashMap::estimate_size(word_count)), buffer_(new char[size_])
  {
    MEMSET(buffer_.get(), 0, size_);
    header()->buffer_size_ = size_;
    header()->capacity_ = ObArrayHashMap::estimate_capacity(word_count);
  }
  int insert(const ObString &word, const ObFTWordCode code)
  {
    uint64_t index = word.hash() % header()->capacity_;
    while (header()->data[index].used) {
      index = (index + 3) % header()->capacity_;
    }
    header()->data[index].code = code;
    const int ret = header()->data[index].word.set_word(word.ptr(), word.length());
    if (OB_SUCCESS == ret) {
      header()->data[index].used = true;
      ++header()->count_;
    }
    return ret;
  }
  int find(const ObFTSingleWord &word, ObFTWordCode &code) const
  {
    uint64_t index = word.get_word().hash() % header()->capacity_;
    while (header()->data[index].used) {
      if (header()->data[index].word == word) {
        code = header()->data[index].code;
        return OB_SUCCESS;
      }
      index = (index + 3) % header()->capacity_;
    }
    return OB_ENTRY_NOT_EXIST;
  }
  ObArrayHashMap::Header *header()
  { return reinterpret_cast<ObArrayHashMap::Header *>(buffer_.get()); }
  const ObArrayHashMap::Header *header() const
  { return reinterpret_cast<const ObArrayHashMap::Header *>(buffer_.get()); }
  const ObArrayHashMap *map() const
  { return reinterpret_cast<const ObArrayHashMap *>(buffer_.get()); }
  const char *data() const { return buffer_.get(); }
  size_t size() const { return size_; }
private:
  size_t size_;
  std::unique_ptr<char[]> buffer_;
};

std::vector<std::string> colliding_dat_keys(const size_t capacity, const size_t bucket,
                                          const size_t length = 4, const size_t count = 7)
{
  std::vector<std::string> keys;
  // Three- and four-byte keys stay distinct over this seed range, and also
  // exercise embedded NUL/high bytes without requiring valid UTF-8.
  for (uint32_t seed = 1; keys.size() < count && seed < 1000000; ++seed) {
    char bytes[4];
    for (size_t i = 0; i < sizeof(bytes); ++i) {
      bytes[i] = static_cast<char>((seed >> (8 * i)) & 0xff);
    }
    const ObString key(length, bytes);
    if (key.hash() % capacity == bucket) {
      keys.emplace_back(bytes, length);
    }
  }
  return keys;
}

void expect_same_dat_lookup(const LegacyDatCodeMap &legacy, const ObArrayHashMap &optimized,
                            const std::string &key, const bool present, const ObFTWordCode value)
{
  ObFTSingleWord word;
  ASSERT_EQ(OB_SUCCESS, word.set_word(key.data(), key.size()));
  ObFTWordCode expected = -77, actual = -77, old_layout_actual = -77;
  const int expected_ret = legacy.find(word, expected);
  EXPECT_EQ(present ? OB_SUCCESS : OB_ENTRY_NOT_EXIST, expected_ret);
  EXPECT_EQ(expected_ret, optimized.find(word, actual));
  EXPECT_EQ(expected_ret, legacy.map()->find(word, old_layout_actual));
  EXPECT_EQ(present ? value : -77, expected);
  EXPECT_EQ(expected, actual);
  EXPECT_EQ(expected, old_layout_actual);
}

TEST(FtsDatCodeMap, collision_and_wrap_probes_keep_original_layout_and_lookup)
{
  // Include capacities divisible by three as well as both other remainders.
  // Keep each probe cycle sparse, as required by the original map algorithm.
  for (const size_t word_count : {size_t(0), size_t(77), size_t(80), size_t(81), size_t(100), size_t(800)}) {
    LegacyDatCodeMap legacy(word_count);
    ObArenaAllocator allocator;
    auto *map = static_cast<ObArrayHashMap *>(allocator.alloc(legacy.size()));
    ASSERT_NE(nullptr, map);
    ASSERT_EQ(OB_SUCCESS, map->init(word_count));
    const size_t capacity = map->capacity();
    std::vector<std::string> inserted;
    std::vector<std::string> missing;
    for (const size_t bucket : {capacity - 1, capacity - 2, capacity - 3, size_t(0)}) {
      SCOPED_TRACE(::testing::Message() << "capacity=" << capacity << ", bucket=" << bucket);
      const auto keys = colliding_dat_keys(capacity, bucket);
      ASSERT_EQ(7U, keys.size());
      for (size_t i = 0; i < 6; ++i) {
        const ObFTWordCode code = static_cast<ObFTWordCode>(inserted.size() * 17 + 1);
        const ObString key(keys[i].size(), keys[i].data());
        ASSERT_EQ(OB_SUCCESS, legacy.insert(key, code));
        ASSERT_EQ(OB_SUCCESS, map->insert(key, code));
        inserted.push_back(keys[i]);
        // Compare the whole packed block after every insertion, so a different
        // collision order cannot pass only because lookup changed with it.
        EXPECT_EQ(0, MEMCMP(legacy.data(), map, legacy.size()));
        expect_same_dat_lookup(legacy, *map, keys[i], true, code);
      }
      missing.push_back(keys.back());
      if (bucket == capacity - 1) {
        EXPECT_TRUE(legacy.header()->data[2].used);
      } else if (bucket == capacity - 2) {
        EXPECT_TRUE(legacy.header()->data[1].used);
      }
    }
    for (size_t i = 0; i < inserted.size(); ++i) {
      expect_same_dat_lookup(legacy, *map, inserted[i], true, static_cast<ObFTWordCode>(i * 17 + 1));
    }
    for (const auto &key : missing) {
      expect_same_dat_lookup(legacy, *map, key, false, 0);
    }
  }
}

TEST(FtsDatCodeMap, three_byte_utf8_uses_original_buckets_and_codes)
{
  LegacyDatCodeMap legacy(65536);
  ObArenaAllocator allocator;
  auto *map = static_cast<ObArrayHashMap *>(allocator.alloc(legacy.size()));
  ASSERT_NE(nullptr, map);
  ASSERT_EQ(OB_SUCCESS, map->init(65536));
  for (uint32_t scalar = 0x800; scalar <= 0xffff; ++scalar) {
    if (scalar >= 0xd800 && scalar <= 0xdfff) {
      continue;
    }
    const char bytes[] = {static_cast<char>(0xe0 | (scalar >> 12)),
                          static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)),
                          static_cast<char>(0x80 | (scalar & 0x3f))};
    const ObString key(sizeof(bytes), bytes);
    ASSERT_EQ(OB_SUCCESS, legacy.insert(key, scalar));
    ASSERT_EQ(OB_SUCCESS, map->insert(key, scalar));
  }
  EXPECT_EQ(0, MEMCMP(legacy.data(), map, legacy.size()));
  for (uint32_t scalar = 0x800; scalar <= 0xffff; ++scalar) {
    if (scalar >= 0xd800 && scalar <= 0xdfff) {
      continue;
    }
    const char bytes[] = {static_cast<char>(0xe0 | (scalar >> 12)),
                          static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)),
                          static_cast<char>(0x80 | (scalar & 0x3f))};
    expect_same_dat_lookup(legacy, *map, std::string(bytes, sizeof(bytes)), true, scalar);
  }
  // Invalid encodings and embedded NUL remain ordinary binary map keys.
  for (const std::string &key : {std::string("\0\0\0", 3), std::string("\xff\x80\0", 3),
                                  std::string("\xed\xa0\x80", 3), std::string("abc", 3)}) {
    expect_same_dat_lookup(legacy, *map, key, false, 0);
  }
}

TEST(FtsDatCodeMap, three_byte_collisions_wrap_and_support_full_cycle_hits)
{
  LegacyDatCodeMap legacy(0);
  ObArenaAllocator allocator;
  auto *map = static_cast<ObArrayHashMap *>(allocator.alloc(legacy.size()));
  ASSERT_NE(nullptr, map);
  ASSERT_EQ(OB_SUCCESS, map->init(0));
  const size_t capacity = map->capacity();
  const auto keys = colliding_dat_keys(capacity, capacity - 1, 3, capacity + 1);
  ASSERT_EQ(capacity + 1, keys.size());
  for (size_t i = 0; i < capacity; ++i) {
    // A miss must leave the caller's code unchanged, including after wrapping.
    expect_same_dat_lookup(legacy, *map, keys[i], false, 0);
    const ObString key(keys[i].size(), keys[i].data());
    ASSERT_EQ(OB_SUCCESS, legacy.insert(key, i + 1));
    ASSERT_EQ(OB_SUCCESS, map->insert(key, i + 1));
    EXPECT_EQ(0, MEMCMP(legacy.data(), map, legacy.size()));
  }
  // A full table has no missing-key termination contract; every existing key
  // must still be found, including keys at the end of the complete probe cycle.
  for (size_t i = 0; i < capacity; ++i) {
    expect_same_dat_lookup(legacy, *map, keys[i], true, i + 1);
  }
}

TEST(FtsDatCodeMap, three_byte_prefixes_and_other_lengths_remain_distinct)
{
  const std::vector<std::string> keys = {std::string(), "a", "ab", "abc", "abcd", "abcde",
      std::string("\0", 1), std::string("\0\xff", 2), std::string("\0\xff\x80", 3),
      std::string("\0\xff\x80\0", 4), std::string("\0\xff\x80\0\xff", 5)};
  LegacyDatCodeMap legacy(keys.size());
  ObArenaAllocator allocator;
  auto *map = static_cast<ObArrayHashMap *>(allocator.alloc(legacy.size()));
  ASSERT_NE(nullptr, map);
  ASSERT_EQ(OB_SUCCESS, map->init(keys.size()));
  for (size_t i = 0; i < keys.size(); ++i) {
    const ObString key(keys[i].size(), keys[i].c_str());
    ASSERT_EQ(OB_SUCCESS, legacy.insert(key, i + 11));
    ASSERT_EQ(OB_SUCCESS, map->insert(key, i + 11));
  }
  EXPECT_EQ(0, MEMCMP(legacy.data(), map, legacy.size()));
  for (size_t i = 0; i < keys.size(); ++i) {
    expect_same_dat_lookup(legacy, *map, keys[i], true, i + 11);
  }
  for (const std::string &key : {"b", "ac", "abd", "abce", "abcdf"}) {
    expect_same_dat_lookup(legacy, *map, key, false, 0);
  }
  // The original pointer-identity shortcut also accepts a live stored word.
  for (size_t i = 0; i < map->capacity(); ++i) {
    const auto &entry = legacy.header()->data[i];
    if (entry.used) {
      ObFTWordCode code = -77;
      ASSERT_EQ(OB_SUCCESS, legacy.map()->find(entry.word, code));
      EXPECT_EQ(entry.code, code);
    }
  }
}

TEST(FtsDatCodeMap, three_byte_lookup_needs_no_bytes_outside_packed_objects)
{
#if defined(__linux__)
  const int64_t page_size = sysconf(_SC_PAGESIZE);
  ASSERT_GE(page_size, 512);
  void *mapping = mmap(nullptr, 3 * page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(MAP_FAILED, mapping);
  struct MappingGuard
  {
    void *mapping_;
    int64_t size_;
    ~MappingGuard() { munmap(mapping_, size_); }
  } cleanup{mapping, 3 * page_size};
  char *readable = static_cast<char *>(mapping) + page_size;
  ASSERT_EQ(0, mprotect(readable, page_size, PROT_READ | PROT_WRITE));
  LegacyDatCodeMap legacy(0);
  ObArenaAllocator allocator;
  auto *map = static_cast<ObArrayHashMap *>(allocator.alloc(legacy.size()));
  ASSERT_NE(nullptr, map);
  ASSERT_EQ(OB_SUCCESS, map->init(0));
  for (const char *bytes : {"abc", "\xe4\xb8\xad", "\0\xff\x80"}) {
    const ObString key(3, bytes);
    ASSERT_EQ(OB_SUCCESS, legacy.insert(key, 51));
    ASSERT_EQ(OB_SUCCESS, map->insert(key, 51));
  }
  for (char *position : {readable, readable + page_size - sizeof(ObFTSingleWord)}) {
    auto *word = new (position) ObFTSingleWord();
    for (const char *bytes : {"abc", "\xe4\xb8\xad", "\0\xff\x80", "abd"}) {
      ASSERT_EQ(OB_SUCCESS, word->set_word(bytes, 3));
      // Unused packed character bytes must never affect the three-byte key.
      word->word[3] = '\x55';
      word->word[4] = '\xaa';
      ObFTWordCode expected = -77, actual = -77;
      ASSERT_EQ(legacy.find(*word, expected), map->find(*word, actual));
      EXPECT_EQ(expected, actual);
    }
    word->~ObFTSingleWord();
  }
#endif
}

} // namespace
} // namespace storage
} // namespace oceanbase
