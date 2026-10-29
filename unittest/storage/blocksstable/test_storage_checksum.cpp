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
#include <array>
#include <limits>
#include <vector>
#include "lib/checksum/ob_crc64.h"
#include "storage/blocksstable/ob_micro_block_checksum_helper.h"
#if defined(__GNUC__) && defined(__x86_64__)
#include <cpuid.h>
#elif defined(_WIN32) && defined(_M_X64)
#include <intrin.h>
#endif
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace oceanbase
{
namespace blocksstable
{
namespace
{
using namespace common;

const std::array<uint64_t, 8> STORAGE_CHECKSUM_SEEDS = {{
    0, 1, UINT32_MAX, uint64_t(1) << 32, uint64_t(1) << 63,
    UINT64_MAX, 0x0123456789abcdefULL, 0xfedcba9876543210ULL}};

// Independent bitwise CRC32C reference. In particular, a nonempty update
// consumes only the low 32 seed bits; an empty/invalid update retains all 64.
uint64_t storage_checksum_reference(const uint64_t seed, const char *data, const int64_t size)
{
  uint64_t result = seed;
  if (nullptr != data && size > 0) {
    uint32_t crc = static_cast<uint32_t>(seed);
    for (int64_t i = 0; i < size; ++i) {
      crc ^= static_cast<uint8_t>(data[i]);
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ (0x82f63b78U & (0U - (crc & 1U)));
      }
    }
    result = crc;
  }
  return result;
}

bool storage_checksum_hardware_available()
{
#if defined(__GNUC__) && defined(__x86_64__)
  unsigned int a = 0, b = 0, c = 0, d = 0;
  return __get_cpuid(1, &a, &b, &c, &d) && (c & bit_SSE4_2) != 0;
#elif defined(_WIN32) && defined(_M_X64)
  int registers[4];
  __cpuid(registers, 1);
  return (registers[2] & (1 << 20)) != 0;
#elif defined(__aarch64__)
  return true; // The existing ARM dispatcher selects the hardware path.
#else
  return false;
#endif
}

void fill_storage_checksum_data(char *data, const int64_t size)
{
  uint64_t state = 0xa91b0482cb153e7dULL;
  for (int64_t i = 0; i < size; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    data[i] = static_cast<char>(state);
  }
}

TEST(StorageChecksum, every_short_length_alignment_and_seed_matches_scalar)
{
  std::vector<char> data(512 + 32);
  fill_storage_checksum_data(data.data(), data.size());
  const bool hardware = storage_checksum_hardware_available();
  if (!hardware) { return; }
  for (int64_t offset = 0; offset < 32; ++offset) {
    for (int64_t size = 0; size <= 512; ++size) {
      for (const uint64_t seed : STORAGE_CHECKSUM_SEEDS) {
        const uint64_t expected = storage_checksum_reference(seed, data.data() + offset, size);
        ASSERT_EQ(expected, ob_crc64_sse42(seed, data.data() + offset, size))
            << "offset=" << offset << " size=" << size << " seed=" << seed;
        if (hardware) {
          ASSERT_EQ(expected, crc64_sse42(seed, data.data() + offset, size))
              << "offset=" << offset << " size=" << size << " seed=" << seed;
        }
      }
    }
  }
}

TEST(StorageChecksum, long_lengths_and_stream_boundaries_match_scalar)
{
  std::vector<char> data(65536 + 32);
  fill_storage_checksum_data(data.data(), data.size());
  const bool hardware = storage_checksum_hardware_available();
  if (!hardware) { return; }
  for (int64_t offset = 0; offset < 16; ++offset) {
    for (const int64_t size : {513, 1023, 1024, 1025, 4095, 4096, 4097, 65535, 65536}) {
      for (const uint64_t seed : STORAGE_CHECKSUM_SEEDS) {
        const char *input = data.data() + offset;
        const uint64_t expected = storage_checksum_reference(seed, input, size);
        ASSERT_EQ(expected, ob_crc64_sse42(seed, input, size));
        if (hardware) {
          ASSERT_EQ(expected, crc64_sse42(seed, input, size));
        }
      }
    }
    for (const uint64_t seed : STORAGE_CHECKSUM_SEEDS) {
      const char *input = data.data() + offset;
      const uint64_t expected = storage_checksum_reference(seed, input, 129);
      for (int64_t split = 0; split <= 129; ++split) {
        const uint64_t first = ob_crc64_sse42(seed, input, split);
        ASSERT_EQ(expected, ob_crc64_sse42(first, input + split, 129 - split));
        if (hardware) {
          const uint64_t hardware_first = crc64_sse42(seed, input, split);
          ASSERT_EQ(expected, crc64_sse42(hardware_first, input + split, 129 - split));
        }
      }
    }
  }
}

TEST(StorageChecksum, null_and_nonpositive_updates_preserve_the_entire_seed)
{
  const bool hardware = storage_checksum_hardware_available();
  // The unchanged software fallback has a different historical contract for
  // invalid updates and upper seed bits; exercise only the hardware path here.
  if (!hardware) { return; }
  const char data = 'a';
  for (const uint64_t seed : STORAGE_CHECKSUM_SEEDS) {
    for (const int64_t size : {std::numeric_limits<int64_t>::min(), int64_t(-1), int64_t(0),
                              int64_t(1), std::numeric_limits<int64_t>::max()}) {
      ASSERT_EQ(seed, ob_crc64_sse42(seed, nullptr, size));
      if (hardware) {
        ASSERT_EQ(seed, crc64_sse42(seed, nullptr, size));
      }
      if (size <= 0) {
        ASSERT_EQ(seed, ob_crc64_sse42(seed, &data, size));
        if (hardware) {
          ASSERT_EQ(seed, crc64_sse42(seed, &data, size));
        }
      }
    }
  }
}

TEST(StorageChecksum, guarded_inputs_require_no_prefix_or_suffix_padding)
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
  fill_storage_checksum_data(readable, page_size);
  const bool hardware = storage_checksum_hardware_available();
  if (!hardware) { return; }
  for (const uint64_t seed : STORAGE_CHECKSUM_SEEDS) {
    const char *inaccessible = static_cast<const char *>(mapping);
    for (const int64_t size : {int64_t(-1), int64_t(0)}) {
      ASSERT_EQ(seed, ob_crc64_sse42(seed, inaccessible, size));
      if (hardware) {
        ASSERT_EQ(seed, crc64_sse42(seed, inaccessible, size));
      }
    }
  }
  for (int64_t size = 1; size <= 512; ++size) {
    for (const char *input : {readable, readable + page_size - size}) {
      for (const uint64_t seed : STORAGE_CHECKSUM_SEEDS) {
        const uint64_t expected = storage_checksum_reference(seed, input, size);
        ASSERT_EQ(expected, ob_crc64_sse42(seed, input, size));
        if (hardware) {
          ASSERT_EQ(expected, crc64_sse42(seed, input, size));
        }
      }
    }
  }
#endif
}

TEST(StorageChecksum, datum_and_microblock_row_checksums_keep_pack_bytes_and_order)
{
  std::array<char, 67> payload;
  fill_storage_checksum_data(payload.data(), payload.size());
  ObStorageDatum datums[6];
  datums[0].set_string(payload.data() + 1, 3);
  datums[1].set_string(payload.data() + 4, 16);
  datums[2].set_int(-123);
  datums[3].set_uint(UINT64_MAX);
  datums[4].set_null();
  datums[5].set_nop();
  ObMicroBlockChecksumHelper helper;
  uint64_t expected = 0;
  for (int iteration = 0; iteration < 5; ++iteration) {
    for (const ObStorageDatum &datum : datums) {
      uint64_t next = storage_checksum_reference(expected,
          reinterpret_cast<const char *>(&datum.pack_), sizeof(datum.pack_));
      next = storage_checksum_reference(next, datum.ptr_, datum.len_);
      ASSERT_EQ(next, static_cast<uint64_t>(datum.checksum(expected)));
      expected = next;
    }
    ASSERT_EQ(OB_SUCCESS, helper.cal_row_checksum(datums, 6));
    ASSERT_EQ(expected, static_cast<uint64_t>(helper.get_row_checksum()));
  }
  helper.reuse();
  ASSERT_EQ(0, helper.get_row_checksum());
}

} // namespace
} // namespace blocksstable
} // namespace oceanbase
