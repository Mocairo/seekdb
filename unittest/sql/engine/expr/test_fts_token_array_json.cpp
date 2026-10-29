/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "common/json_type/ob_json_bin.h"
#include "common/json_type/ob_json_tree.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_json_func_helper.h"
#include "sql/engine/expr/ob_expr_operator.h"
#define private public
#include "sql/engine/expr/ob_expr_tokenize.h"
#undef private

namespace oceanbase
{
namespace sql
{
namespace
{
using namespace common;
using namespace storage;

// Unlike an arena, this allocator exposes an accidentally freed output buffer.
class TokenArrayScratchAllocator : public ObIAllocator
{
public:
  explicit TokenArrayScratchAllocator(const bool retain_freed = false)
    : retain_freed_(retain_freed)
  {}
  ~TokenArrayScratchAllocator() override
  {
    for (const auto &entry : active_) { std::free(entry.first); }
    for (void *pointer : retired_) { std::free(pointer); }
  }
  void *alloc(const int64_t size) override
  {
    void *pointer = nullptr;
    if (requests_++ == fail_at_) {
      ++failures_;
    } else if (size > 0 && nullptr != (pointer = std::malloc(size))) {
      active_[pointer] = size;
    }
    return pointer;
  }
  void *alloc(const int64_t size, const ObMemAttr &) override { return alloc(size); }
  void free(void *pointer) override
  {
    if (nullptr != pointer) {
      const auto found = active_.find(pointer);
      if (found == active_.end()) {
        invalid_free_ = true;
      } else {
        std::memset(pointer, 0xA5, found->second);
        active_.erase(found);
        if (retain_freed_) { retired_.push_back(pointer); }
        else { std::free(pointer); }
      }
    }
  }
  bool owns(const char *pointer, const int64_t length) const
  {
    const auto found = active_.find(const_cast<char *>(pointer));
    return length >= 0 && found != active_.end() && length <= found->second;
  }
  void fail_at(const int64_t request) { fail_at_ = request; }
  int64_t requests() const { return requests_; }
  int64_t failures() const { return failures_; }
  size_t active_count() const { return active_.size(); }
  bool invalid_free() const { return invalid_free_; }
private:
  std::map<void *, int64_t> active_;
  std::vector<void *> retired_;
  bool retain_freed_;
  bool invalid_free_ = false;
  int64_t requests_ = 0;
  int64_t failures_ = 0;
  int64_t fail_at_ = -1;
};

int fill_token_map(const std::vector<std::string> &tokens, ObFTWordMap &words,
    const ObCollationType collation = CS_TYPE_UTF8MB4_BIN)
{
  const int64_t bucket_count = tokens.size() < 8 ? 17 : tokens.size() * 2 + 1;
  int ret = words.create(bucket_count, "FTArrayTest");
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(collation);
  for (size_t i = 0; OB_SUCC(ret) && i < tokens.size(); ++i) {
    // Repeated/collation-equivalent words update frequency, retaining the map's
    // existing spelling and order exactly as the DEFAULT tree path does.
    const ObFTWord key(tokens[i].length(), tokens[i].data(), meta);
    ret = words.set_refactored(key, i + 1, 1);
  }
  return ret;
}

void check_array_order_and_bytes(const ObFTWordMap &words, ObIJsonBase &json,
    ObIAllocator &allocator)
{
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, json.json_type());
  ASSERT_EQ(static_cast<uint64_t>(words.size()), json.element_count());
  uint64_t index = 0;
  for (auto it = words.begin(); it != words.end(); ++it, ++index) {
    ObJsonBin child(&allocator);
    ObIJsonBase *value = json.is_bin() ? &child : nullptr;
    ASSERT_EQ(OB_SUCCESS, json.get_array_element(index, value));
    ASSERT_NE(nullptr, value);
    EXPECT_EQ(ObJsonNodeType::J_STRING, value->json_type());
    EXPECT_EQ(it->first.get_word().get_string(),
        ObString(value->get_data_length(), value->get_data()));
  }
}

void check_against_tree(const std::vector<std::string> &tokens,
    const ObCollationType collation = CS_TYPE_UTF8MB4_BIN,
    const int expected_entry_width = -1)
{
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, fill_token_map(tokens, words, collation));
  ObArenaAllocator allocator("FTArrayTest");
  ObFTParseHelper helper;
  ASSERT_EQ(OB_SUCCESS, helper.init(&allocator, ObString("space.1"), ObFTParserProperty()));
  ObIJsonBase *tree = nullptr;
  ObIJsonBase *binary = nullptr;
  // Keep the original object-tree construction as an independent oracle.
  ASSERT_EQ(OB_SUCCESS, helper.make_token_array_json(words, tree));
  ASSERT_EQ(OB_SUCCESS, helper.make_token_array_json_bin(words, binary));
  ASSERT_NE(nullptr, tree);
  ASSERT_NE(nullptr, binary);
  ASSERT_TRUE(binary->is_bin());
  int comparison = 1;
  ASSERT_EQ(OB_SUCCESS, binary->compare(*tree, comparison));
  EXPECT_EQ(0, comparison);
  check_array_order_and_bytes(words, *tree, allocator);
  check_array_order_and_bytes(words, *binary, allocator);
  for (const bool pretty : {false, true}) {
    ObJsonBuffer expected(&allocator);
    ObJsonBuffer actual(&allocator);
    ASSERT_EQ(OB_SUCCESS, tree->print(expected, true, 0, pretty));
    ASSERT_EQ(OB_SUCCESS, binary->print(actual, true, 0, pretty));
    EXPECT_EQ(expected.string(), actual.string());
  }
  ObString tree_data;
  ObString binary_data;
  ASSERT_EQ(OB_SUCCESS, tree->get_raw_binary(tree_data, &allocator));
  ASSERT_EQ(OB_SUCCESS, binary->get_raw_binary(binary_data, &allocator));
  // TOKENIZE returns a JSON datum whose binary size is observable through
  // JSON_STORAGE_SIZE; preserve the tree serializer's bytes as well as values.
  EXPECT_EQ(tree_data.length(), binary_data.length());
  EXPECT_EQ(tree_data, binary_data);
  ObJsonBin serialized_tree(&allocator);
  ObJsonBin reloaded(&allocator);
  ASSERT_EQ(OB_SUCCESS, serialized_tree.reset(tree_data, 0, nullptr));
  ASSERT_EQ(OB_SUCCESS, reloaded.reset(binary_data, 0, nullptr));
  ASSERT_EQ(OB_SUCCESS, reloaded.compare(serialized_tree, comparison));
  EXPECT_EQ(0, comparison);
  check_array_order_and_bytes(words, reloaded, allocator);
  if (expected_entry_width >= 0) {
    ASSERT_GT(binary_data.length(), static_cast<int64_t>(sizeof(ObJsonBinDocHeader)));
    const auto *header = reinterpret_cast<const ObJsonBinHeader *>(
        binary_data.ptr() + sizeof(ObJsonBinDocHeader));
    EXPECT_EQ(expected_entry_width, static_cast<int>(header->entry_size_));
    EXPECT_EQ(expected_entry_width, static_cast<int>(header->obj_size_size_));
    EXPECT_EQ(ObJsonVar::get_var_type(static_cast<uint64_t>(words.size())),
        static_cast<uint8_t>(header->count_size_));
  }
}

void release_binary(ObIJsonBase *&result, TokenArrayScratchAllocator &allocator)
{
  ASSERT_NE(nullptr, result);
  ObString data;
  ASSERT_EQ(OB_SUCCESS, result->get_raw_binary(data, &allocator));
  ASSERT_TRUE(allocator.owns(data.ptr(), data.length()));
  auto *binary = static_cast<ObJsonBin *>(result);
  OB_DELETEx(ObJsonBin, &allocator, binary);
  allocator.free(data.ptr());
  result = nullptr;
}

TEST(FTTokenArrayJson, tree_differential_preserves_empty_unicode_controls_and_collation)
{
  check_against_tree({});
  check_against_tree({""});
  check_against_tree({"one", "two", "one", "three", "two"});
  check_against_tree({"", "quote\"", "slash\\", "line\n", "tab\t", "\r\b\f",
      std::string("zero\0byte", 9), std::string("\x01\x1f", 2),
      "\xe4\xb8\xad\xe6\x96\x87", "\xf0\x9f\x98\x80", "a\xc3\xa9"});
  check_against_tree({"First", "FIRST", "first", "Second", "SECOND", "caf\xc3\xa9", "cafe"},
      CS_TYPE_UTF8MB4_GENERAL_CI);
  // Ordinary token lengths can also cross the tree serializer's conservative
  // metadata estimate even when the smallest possible encoding would not.
  check_against_tree({std::string(80, 'a'), std::string(80, 'b'), std::string(79, 'c')},
      CS_TYPE_UTF8MB4_BIN, JBLS_UINT16);
}

TEST(FTTokenArrayJson, tree_differential_crosses_string_length_and_container_width_boundaries)
{
  for (const int64_t length : {0, 1, 126, 127, 128, 129, 16382, 16383, 16384, 16385}) {
    SCOPED_TRACE(length);
    check_against_tree({std::string(length, 's')});
  }
  // A width change also widens the offset table and object-size field. The
  // final layout must be recomputed before writing any entry offsets.
  for (const int64_t length : {245, 246, 247, 248, 65522, 65523, 65524, 65525}) {
    SCOPED_TRACE(length);
    const int width = length <= 245 ? JBLS_UINT8 : length <= 65522 ? JBLS_UINT16 : JBLS_UINT32;
    check_against_tree({std::string(length, 'w')}, CS_TYPE_UTF8MB4_BIN, width);
  }
  for (const int64_t count : {254, 255, 256, 257, 65535, 65536}) {
    SCOPED_TRACE(count);
    std::vector<std::string> tokens;
    tokens.reserve(count);
    for (int64_t i = 0; i < count; ++i) { tokens.push_back("token" + std::to_string(i)); }
    check_against_tree(tokens, CS_TYPE_UTF8MB4_BIN, count < 65535 ? JBLS_UINT16 : JBLS_UINT32);
  }
}

TEST(FTTokenArrayJson, binary_owns_bytes_after_input_and_helper_destruction)
{
  for (const bool retain_freed : {false, true}) {
    TokenArrayScratchAllocator allocator(retain_freed);
    ObIJsonBase *result = nullptr;
    {
      const std::vector<std::string> tokens = {"original", std::string("NUL\0tail", 8)};
      ObFTWordMap words;
      ASSERT_EQ(OB_SUCCESS, fill_token_map(tokens, words));
      ObFTParseHelper helper;
      ASSERT_EQ(OB_SUCCESS, helper.init(&allocator, ObString("space.1"), ObFTParserProperty()));
      ASSERT_EQ(OB_SUCCESS, helper.make_token_array_json_bin(words, result));
    }
    // A live allocation check precedes dereferencing the result's bytes, so
    // the true-free variant can safely detect a missing buffer detach.
    ObString data;
    ASSERT_EQ(OB_SUCCESS, result->get_raw_binary(data, &allocator));
    ASSERT_TRUE(allocator.owns(data.ptr(), data.length()));
    ObArenaAllocator expected_allocator("FTArrayTest");
    const std::vector<std::string> tokens = {"original", std::string("NUL\0tail", 8)};
    ObFTWordMap expected_words;
    ASSERT_EQ(OB_SUCCESS, fill_token_map(tokens, expected_words));
    check_array_order_and_bytes(expected_words, *result, expected_allocator);
    release_binary(result, allocator);
    EXPECT_EQ(0U, allocator.active_count());
    EXPECT_FALSE(allocator.invalid_free());
  }
}

TEST(FTTokenArrayJson, every_allocation_failure_releases_partial_state)
{
  const std::vector<std::string> tokens = {"first", "second", std::string(2048, 't')};
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, fill_token_map(tokens, words));
  int64_t allocations = 0;
  {
    TokenArrayScratchAllocator allocator;
    ObFTParseHelper helper;
    ASSERT_EQ(OB_SUCCESS, helper.init(&allocator, ObString("space.1"), ObFTParserProperty()));
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_SUCCESS, helper.make_token_array_json_bin(words, result));
    allocations = allocator.requests();
    release_binary(result, allocator);
    EXPECT_EQ(0U, allocator.active_count());
  }
  ASSERT_GT(allocations, 0);
  for (int64_t failure = 0; failure < allocations; ++failure) {
    SCOPED_TRACE(failure);
    TokenArrayScratchAllocator allocator(true);
    ObFTParseHelper helper;
    ASSERT_EQ(OB_SUCCESS, helper.init(&allocator, ObString("space.1"), ObFTParserProperty()));
    allocator.fail_at(failure);
    ObIJsonBase *result = nullptr;
    EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, helper.make_token_array_json_bin(words, result));
    EXPECT_EQ(nullptr, result);
    EXPECT_EQ(1, allocator.failures());
    EXPECT_EQ(failure + 1, allocator.requests());
    EXPECT_EQ(0U, allocator.active_count());
    EXPECT_FALSE(allocator.invalid_free());
    // The same helper can be retried after a failed build without stale state.
    allocator.fail_at(-1);
    ASSERT_EQ(OB_SUCCESS, helper.make_token_array_json_bin(words, result));
    release_binary(result, allocator);
    EXPECT_EQ(0U, allocator.active_count());
    EXPECT_FALSE(allocator.invalid_free());
  }
}

TEST(FTTokenArrayJson, oversized_layout_fails_before_allocating_or_reading_string_payloads)
{
  // ObDatum lengths occupy 29 bits. Four individually representable lengths
  // exercise payload accumulation, minimum layout, and widened layout overflow.
  const int64_t max_datum_length = (1LL << 29) - 1;
  const std::vector<std::string> tokens = {"one", "two", "three", "four"};
  for (const int64_t length : {max_datum_length, max_datum_length - 12, max_datum_length - 15}) {
    SCOPED_TRACE(length);
    ObFTWordMap words;
    ASSERT_EQ(OB_SUCCESS, fill_token_map(tokens, words));
    // Only describe large payloads: changing the borrowed datum lengths after
    // insertion avoids huge test allocations. Sizing must reject the layout
    // before attempting to read these deliberately unavailable payload bytes.
    for (auto it = words.begin(); it != words.end(); ++it) {
      ObDatum &word = const_cast<ObDatum &>(it->first.get_word());
      word.set_string(word.ptr_, length);
      ASSERT_EQ(length, static_cast<int64_t>(word.len_));
    }
    TokenArrayScratchAllocator allocator;
    allocator.fail_at(0);
    ObFTParseHelper helper;
    ASSERT_EQ(OB_SUCCESS, helper.init(&allocator, ObString("space.1"), ObFTParserProperty()));
    ObIJsonBase *result = nullptr;
    EXPECT_EQ(OB_SIZE_OVERFLOW, helper.make_token_array_json_bin(words, result));
    EXPECT_EQ(nullptr, result);
    EXPECT_EQ(0, allocator.requests());
    EXPECT_EQ(0U, allocator.active_count());
  }
}

struct TokenArrayResultFrame
{
  explicit TokenArrayResultFrame(const int64_t index, const bool has_header)
  {
    MEMSET(memory_, 0, sizeof(memory_));
    expr_.frame_idx_ = index;
    expr_.datum_off_ = 0;
    expr_.dyn_buf_header_offset_ = 32;
    expr_.res_buf_off_ = 64;
    expr_.res_buf_len_ = sizeof(memory_) - expr_.res_buf_off_;
    expr_.datum_meta_.type_ = ObJsonType;
    expr_.obj_meta_.set_json();
    if (has_header) { expr_.obj_meta_.set_has_lob_header(); }
    expr_.batch_idx_mask_ = 0;
    new (memory_) ObDatum();
  }
  ObDatum &datum() { return *reinterpret_cast<ObDatum *>(memory_); }
  ObExpr expr_;
  alignas(8) char memory_[256];
};

TEST(FTTokenArrayJson, packed_results_survive_scratch_release_and_repeated_evaluation)
{
  for (const bool has_header : {false, true}) {
    ObArenaAllocator allocator("FTArrayPack");
    ObExecContext exec_ctx(allocator);
    ObEvalCtx eval_ctx(exec_ctx);
    TokenArrayResultFrame first(0, has_header), second(1, has_header), third(2, has_header);
    TokenArrayResultFrame *result_frames[] = {&first, &second, &third};
    char *frames[] = {first.memory_, second.memory_, third.memory_};
    eval_ctx.frames_ = frames;
    std::vector<std::string> expected;
    for (int64_t i = 0; i < 3; ++i) {
      // The middle result exceeds the frame's reserved buffer and exercises
      // expression-owned dynamic result memory as well as the small case.
      const std::vector<std::string> tokens = {"generation" + std::to_string(i),
          std::string(i == 1 ? 4096 : 8, 'a' + i), std::string("x\0y", 3)};
      {
        TokenArrayScratchAllocator scratch(true);
        ObFTWordMap words;
        ASSERT_EQ(OB_SUCCESS, fill_token_map(tokens, words));
        ObFTParseHelper helper;
        ASSERT_EQ(OB_SUCCESS, helper.init(&scratch, ObString("space.1"), ObFTParserProperty()));
        ObIJsonBase *result = nullptr;
        ASSERT_EQ(OB_SUCCESS, helper.make_token_array_json_bin(words, result));
        ObJsonBuffer printed(&allocator);
        ASSERT_EQ(OB_SUCCESS, result->print(printed, false));
        expected.emplace_back(printed.ptr(), printed.length());
        ASSERT_EQ(OB_SUCCESS, ObJsonExprHelper::pack_json_res(result_frames[i]->expr_,
            eval_ctx, scratch, result, result_frames[i]->datum()));
        release_binary(result, scratch);
        EXPECT_EQ(0U, scratch.active_count());
      }
      // Every earlier packed datum must remain valid after the temporary
      // allocator and its source JSON have gone away and a later result exists.
      for (int64_t previous = 0; previous <= i; ++previous) {
        const ObString packed = result_frames[previous]->datum().get_string();
        ObLobLocatorV2 locator(packed, has_header);
        ObString data;
        ASSERT_EQ(OB_SUCCESS, locator.get_inrow_data(data));
        ObJsonBin decoded(&allocator);
        ASSERT_EQ(OB_SUCCESS, decoded.reset(data, 0, nullptr));
        ObJsonBuffer printed(&allocator);
        ASSERT_EQ(OB_SUCCESS, decoded.print(printed, false));
        EXPECT_EQ(expected[previous], std::string(printed.ptr(), printed.length()));
      }
    }
  }
}

TEST(FTTokenArrayJson, default_tokenizer_returns_valid_binary_after_local_map_destruction)
{
  ObArenaAllocator allocator("FTArrayTest");
  ObIJsonBase *result = nullptr;
  {
    ObExprTokenize::TokenizeParam param(allocator);
    ASSERT_EQ(OB_SUCCESS, ObExprTokenize::construct_ft_parser_inner_name(ObString("ngram"), param));
    ASSERT_EQ(OB_SUCCESS, param.reform_parser_properties(ObString("{\"ngram_token_size\":1}")));
    param.meta_.set_varchar();
    param.meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    const std::string input("aaaa");
    param.fulltext_ = ObString(input.length(), input.data());
    param.output_mode_ = ObExprTokenize::TokenizeParam::DEFAULT;
    ASSERT_EQ(OB_SUCCESS, ObExprTokenize::tokenize_fulltext(param, param.output_mode_, allocator, result));
  }
  ASSERT_NE(nullptr, result);
  ASSERT_TRUE(result->is_bin());
  ObJsonBuffer printed(&allocator);
  ASSERT_EQ(OB_SUCCESS, result->print(printed, false));
  EXPECT_EQ(ObString("[\"a\"]"), printed.string());
}
} // namespace
} // namespace sql
} // namespace oceanbase
