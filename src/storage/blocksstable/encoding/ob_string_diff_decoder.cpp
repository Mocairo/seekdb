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

#define USING_LOG_PREFIX STORAGE

#include "ob_string_diff_decoder.h"

#include "ob_raw_decoder.h"
#include "lib/utility/ob_target_specific.h"

#if defined(__GNUC__) && defined(__x86_64__)
#include <cpuid.h>
#include <tmmintrin.h>
#endif

namespace oceanbase
{
namespace blocksstable
{
using namespace common;
const ObColumnHeader::Type ObStringDiffDecoder::type_;

namespace
{
#if defined(__GNUC__) && defined(__x86_64__)
bool short_string_diff_simd_supported()
{
  // Keep the engine's CPU/OS feature gate, and check SSSE3 itself: its CPUID
  // feature bit is independent of the SSE4.2 bit used by ObTargetArch.
  static const bool has_ssse3 = []() {
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    return __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & bit_SSSE3) != 0;
  }();
  return is_arch_supported(ObTargetArch::SSE42) && has_ssse3;
}

template <int64_t PACKED_LENGTH>
__attribute__((target("ssse3")))
void decode_short_string_diff_rows(
    const char *col_data,
    const int32_t *row_ids,
    const int64_t row_cap,
    const uint16_t string_size,
    const unsigned char *common_bytes,
    const unsigned char *shuffle_bytes,
    char *buf,
    ObDatum *datums)
{
  const __m128i common = _mm_loadu_si128(reinterpret_cast<const __m128i *>(common_bytes));
  const __m128i shuffle = _mm_loadu_si128(reinterpret_cast<const __m128i *>(shuffle_bytes));
  for (int64_t i = 0; i < row_cap; ++i) {
    // The constant-size copy reads exactly this row's packed payload, including
    // the last physical row. A full-width source load could cross the block.
    __m128i packed = _mm_setzero_si128();
    MEMCPY(&packed, col_data + row_ids[i] * PACKED_LENGTH, PACKED_LENGTH);
    char *str_ptr = buf + i * 16;
    _mm_storeu_si128(reinterpret_cast<__m128i *>(str_ptr),
        _mm_or_si128(_mm_shuffle_epi8(packed, shuffle), common));
    datums[i].pack_ = string_size;
    datums[i].ptr_ = str_ptr;
  }
}

int decode_short_string_diff(
    const ObColumnDecoderCtx &ctx,
    const ObStringDiffHeader &header,
    const int32_t *row_ids,
    const int64_t row_cap,
    ObDatum *datums)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(ctx.allocator_->alloc(16 * row_cap));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    unsigned char common_bytes[16] = {};
    unsigned char shuffle_bytes[16];
    MEMSET(shuffle_bytes, 0x80, sizeof(shuffle_bytes));
    header.copy_string(ObStringDiffHeader::LeftToRight(), std::logical_not<uint8_t>(),
        header.common_data(), common_bytes);
    int64_t full_pos = 0;
    int64_t packed_pos = 0;
    for (int64_t i = 0; i < header.diff_desc_cnt_; ++i) {
      const ObStringDiffHeader::DiffDesc &desc = header.diff_descs_[i];
      if (desc.diff_) {
        for (int64_t j = 0; j < desc.count_; ++j) {
          shuffle_bytes[full_pos + j] = static_cast<unsigned char>(packed_pos++);
        }
      }
      full_pos += desc.count_;
    }
    using DecodeFunc = void (*)(const char *, const int32_t *, const int64_t,
        const uint16_t, const unsigned char *, const unsigned char *, char *, ObDatum *);
    static const DecodeFunc decode_funcs[] = {
        decode_short_string_diff_rows<1>, decode_short_string_diff_rows<2>,
        decode_short_string_diff_rows<3>, decode_short_string_diff_rows<4>,
        decode_short_string_diff_rows<5>, decode_short_string_diff_rows<6>,
        decode_short_string_diff_rows<7>, decode_short_string_diff_rows<8>,
        decode_short_string_diff_rows<9>, decode_short_string_diff_rows<10>,
        decode_short_string_diff_rows<11>, decode_short_string_diff_rows<12>,
        decode_short_string_diff_rows<13>, decode_short_string_diff_rows<14>,
        decode_short_string_diff_rows<15>, decode_short_string_diff_rows<16>};
    const char *col_data = reinterpret_cast<const char *>(&header) + ctx.col_header_->length_;
    decode_funcs[header.length_ - 1](col_data, row_ids, row_cap, header.string_size_,
        common_bytes, shuffle_bytes, buf, datums);
  }
  return ret;
}
#endif
} // namespace

ObStringDiffDecoder::ObStringDiffDecoder() : header_(NULL)
{
}

ObStringDiffDecoder::~ObStringDiffDecoder()
{
}

int ObStringDiffDecoder::decode(const ObColumnDecoderCtx &ctx, common::ObDatum &datum, const int64_t row_id,
    const ObBitStream &bs, const char *data, const int64_t len) const
{
  int ret = OB_SUCCESS;
  uint64_t val = STORED_NOT_EXT;
  ObBitStream fix_bs;
  const char *col_data = reinterpret_cast<const char *>(header_) + ctx.col_header_->length_;
  int64_t data_offset = 0;

  if (!is_inited()) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(nullptr == data || len < 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else {

    // read extend value bit
    if (ctx.has_extend_value()) {
      if (ctx.is_fix_length()) {
        data_offset = ctx.micro_block_header_->row_count_ * ctx.micro_block_header_->extend_value_bit_;
        data_offset = (data_offset + CHAR_BIT - 1) / CHAR_BIT;
        if (OB_FAIL(ObBitStream::get(reinterpret_cast<const unsigned char *>(col_data),
            row_id * ctx.micro_block_header_->extend_value_bit_,
            ctx.micro_block_header_->extend_value_bit_, val))) {
        }
      } else {
        if (OB_FAIL(bs.get(ctx.col_header_->extend_value_index_,
                ctx.micro_block_header_->extend_value_bit_, val))) {
        }
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (STORED_NOT_EXT != val) {
    set_stored_ext_value(datum, static_cast<ObStoredExtValue>(val));
  } else {
    const char *cell_data = NULL;
    int64_t cell_len = 0;
    // get cell data offset and length
    if (ctx.is_fix_length()) {
      cell_data = col_data + data_offset + row_id * header_->length_;
    } else {
      if (OB_FAIL(ObRawDecoder::locate_cell_data(cell_data, cell_len, data, len,
              *ctx.micro_block_header_, *ctx.col_header_, *header_))) {
      }
    }

    char *buf = NULL;
    if (OB_SUCC(ret)) {
      const static uint16_t min_buf_size = 128;
      const int64_t buf_size = std::max(header_->string_size_, min_buf_size);
      if (OB_ISNULL(buf = static_cast<char *>(ctx.allocator_->alloc(buf_size)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        header_->copy_string(ObStringDiffHeader::LeftToRight(), std::logical_not<uint8_t>(),
            header_->common_data(), buf);
      }
    }

    if (OB_SUCC(ret)) {
      //int64_t cell_len = header_->length_;
      if (header_->is_hex_packing()) {
        ObHexStringUnpacker unpacker(header_->hex_char_array(),
            reinterpret_cast<const unsigned char *>(cell_data));
        header_->copy_hex_string(unpacker,
            static_cast<void (ObHexStringUnpacker::*)(unsigned char &)>(&ObHexStringUnpacker::unpack),
            reinterpret_cast<unsigned char *>(buf));
      } else {
        header_->copy_string(ObStringDiffHeader::LeftToRight(),
            ObStringDiffHeader::LogicTrue<uint8_t>(),
            cell_data, buf);
      }
      datum.pack_ = header_->string_size_;
      datum.ptr_ = buf;
    }
  }

  return ret;
}

int ObStringDiffDecoder::update_pointer(const char *old_block, const char *cur_block)
{
  int ret = OB_SUCCESS;
  if (!is_inited()) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(old_block) || OB_ISNULL(cur_block)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObIColumnDecoder::update_pointer(header_, old_block, cur_block);
  }
  return ret;
}

// Internal call, not check parameters for performance
int ObStringDiffDecoder::batch_decode(
    const ObColumnDecoderCtx &ctx,
    const ObIRowIndex* row_index,
    const int32_t *row_ids,
    const char **cell_datas,
    const int64_t row_cap,
    common::ObDatum *datums) const
{
  UNUSED(cell_datas);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited())) {
    ret = OB_NOT_INIT;
#if defined(__GNUC__) && defined(__x86_64__)
  } else if (ctx.is_fix_length() && !ctx.has_extend_value()
      && !header_->is_hex_packing() && header_->string_size_ > 0
      && header_->string_size_ <= 16 && header_->length_ > 0
      && header_->length_ <= header_->string_size_ && short_string_diff_simd_supported()) {
    ret = decode_short_string_diff(ctx, *header_, row_ids, row_cap, datums);
#endif
  } else {
    const unsigned char *col_data = reinterpret_cast<const unsigned char *>(header_)
        + ctx.col_header_->length_;
    int64_t data_offset = 0;
    if (ctx.has_extend_value()) {
      if (ctx.is_fix_length()) {
        data_offset = ctx.micro_block_header_->row_count_ * ctx.micro_block_header_->extend_value_bit_;
        data_offset = (data_offset + CHAR_BIT - 1) / CHAR_BIT;
        if (OB_FAIL(set_null_datums_from_fixed_column(
            ctx, row_ids, row_cap, col_data, datums))) {
        }
      } else if (OB_FAIL(set_null_datums_from_var_column(
          ctx, row_index, row_ids, row_cap, datums))) {
      }
    }

    // Fill string data
    const uint16_t string_size = header_->string_size_;
    const static uint16_t min_buf_size = 128;
    const int64_t buf_size = std::max(string_size, min_buf_size);
    char *buf = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(buf = static_cast<char *>(ctx.allocator_->alloc(buf_size * row_cap)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      for (int64_t i = 0; i < row_cap; ++i) {
        header_->copy_string(ObStringDiffHeader::LeftToRight(), std::logical_not<uint8_t>(),
            header_->common_data(), buf + i * buf_size);
      }
    }

    if (OB_FAIL(ret)) {
    } else if (ctx.is_fix_length()) {
      int64_t row_id = 0;
      const unsigned char *cell_data = nullptr;
      char *str_ptr = nullptr;
      for (int64_t i = 0; i < row_cap; ++i) {
        if (ctx.has_extend_value() && datums[i].is_null()) {
          // Skip
        } else {
          row_id = row_ids[i];
          cell_data = col_data + data_offset + row_id * header_->length_;
          str_ptr = buf + i * buf_size;
          if (header_->is_hex_packing()) {
            ObHexStringUnpacker unpacker(header_->hex_char_array(),
                reinterpret_cast<const unsigned char *>(cell_data));
            header_->copy_hex_string(unpacker,
                static_cast<void (ObHexStringUnpacker::*)(unsigned char &)>(&ObHexStringUnpacker::unpack),
                reinterpret_cast<unsigned char *>(str_ptr));
          } else {
            header_->copy_string(
              ObStringDiffHeader::LeftToRight(),
              ObStringDiffHeader::LogicTrue<uint8_t>(),
              cell_data, str_ptr);
          }
          datums[i].pack_ = string_size;
          datums[i].ptr_ = str_ptr;
        }
      }
    } else {
      int64_t row_id = 0;
      const char *row_data = nullptr;
      int64_t row_len = 0;
      const char *cell_data = nullptr;
      int64_t cell_len = 0;
      char *str_ptr = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < row_cap; ++i) {
        row_id = row_ids[i];
        str_ptr = buf + i * buf_size;
        if (ctx.has_extend_value() && datums[i].is_null()) {
          // Do nothing
        } else if (OB_FAIL(locate_row_data(ctx, row_index, row_id, row_data, row_len))) {
        } else if (OB_FAIL(ObRawDecoder::locate_cell_data(cell_data, cell_len, row_data, row_len,
            *ctx.micro_block_header_, *ctx.col_header_, *header_))) {
        } else {
          if (header_->is_hex_packing()) {
            ObHexStringUnpacker unpacker(header_->hex_char_array(),
                reinterpret_cast<const unsigned char *>(cell_data));
            header_->copy_hex_string(unpacker,
                static_cast<void (ObHexStringUnpacker::*)(unsigned char &)>(&ObHexStringUnpacker::unpack),
                reinterpret_cast<unsigned char *>(str_ptr));
          } else {
            header_->copy_string(
              ObStringDiffHeader::LeftToRight(),
              ObStringDiffHeader::LogicTrue<uint8_t>(),
              cell_data, str_ptr);
          }
          datums[i].pack_ = string_size;
          datums[i].ptr_ = str_ptr;
        }
      }
    }
  }
  return ret;
}

} // end namespace blocksstable
} // end namespace oceanbase
