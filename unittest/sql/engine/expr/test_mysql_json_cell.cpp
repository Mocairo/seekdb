/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "common/json_type/ob_json_bin.h"
#include "common/json_type/ob_json_diff.h"
#include "common/json_type/ob_json_parse.h"
#include "common/timezone/ob_time_convert.h"
#include "lib/allocator/page_arena.h"
#include "query/protocol/ob_mysql_protocol_util.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{
namespace
{
using namespace common;
using namespace obmysql;

// Exercise both a real freeing allocator and a retained, poisoned free list.
// An arena's no-op free would hide a result borrowed from a destroyed buffer.
class JsonCellScratchAllocator : public ObIAllocator
{
public:
  explicit JsonCellScratchAllocator(const bool retain_freed = false)
    : retain_freed_(retain_freed)
  {}

  ~JsonCellScratchAllocator() override
  {
    for (const auto &entry : active_) {
      std::free(entry.first);
    }
    for (void *pointer : retired_) {
      std::free(pointer);
    }
  }

  void *alloc(const int64_t size) override
  {
    const int64_t request = requests_++;
    void *pointer = nullptr;
    if (request == fail_at_) {
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
        ++frees_;
        if (retain_freed_) {
          retired_.push_back(pointer);
        } else {
          std::free(pointer);
        }
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
  int64_t frees() const { return frees_; }
  size_t active_count() const { return active_.size(); }
  bool invalid_free() const { return invalid_free_; }

private:
  std::map<void *, int64_t> active_;
  std::vector<void *> retired_;
  bool retain_freed_;
  bool invalid_free_ = false;
  int64_t requests_ = 0;
  int64_t failures_ = 0;
  int64_t frees_ = 0;
  int64_t fail_at_ = -1;
};

// The unchanged tree printer provides an independent wire-format oracle.
int make_mysql_json_cell_input(const std::string &text, ObIAllocator &allocator,
    ObObj &obj, std::string &expected)
{
  int ret = OB_SUCCESS;
  ObJsonNode *tree = nullptr;
  ObString binary;
  if (OB_FAIL(ObJsonParser::get_tree(&allocator, text.data(), text.length(), tree))) {
  } else if (OB_FAIL(tree->get_raw_binary(binary, &allocator))) {
  } else {
    ObJsonBuffer buffer(&allocator);
    if (OB_FAIL(tree->print(buffer, true, binary.length()))) {
    } else {
      expected.assign(buffer.ptr(), buffer.length());
      obj.set_json_value(ObJsonType, binary.ptr(), binary.length());
    }
  }
  return ret;
}

std::string mysql_json_cell_control_text(const int64_t count)
{
  std::string text("[\"");
  for (int64_t i = 0; i < count; ++i) {
    text.append("\\u0001");
  }
  text.append("\"]");
  return text;
}

class MysqlJsonCell : public testing::Test
{
protected:
  int encode(const ObObj &obj, JsonCellScratchAllocator &scratch,
      ObMySQLCellValue &out, const MYSQL_PROTOCOL_TYPE protocol = TEXT)
  {
    return ObSMUtils::build_cell_value(obj, protocol, scratch, out,
        cast_params_, nullptr, session_);
  }

  void check_cell(const ObMySQLCellValue &cell, const std::string &expected,
      const JsonCellScratchAllocator &scratch)
  {
    ASSERT_EQ(ObMySQLCellValueKind::LENENC_BYTES, cell.kind_);
    ASSERT_EQ(static_cast<int64_t>(expected.length()), cell.get_bytes_len());
    ASSERT_FALSE(cell.uses_local_buffer());
    // Check liveness before reading; the true-free variant must not itself
    // dereference a dangling result when detecting a missing buffer detach.
    ASSERT_TRUE(scratch.owns(cell.get_bytes(), cell.get_bytes_len()));
    EXPECT_EQ(expected, std::string(cell.get_bytes(), cell.get_bytes_len()));
  }

  ObDataTypeCastParams cast_params_;
  ObSQLSessionInfo session_;
};

TEST_F(MysqlJsonCell, output_survives_local_destructors_with_freeing_and_poison_allocators)
{
  const char *texts[] = {
      "null", "true", "false", "0", "-123.5", "\"\"", "[]", "{}",
      R"(["one", "two", "three"])",
      R"(["quote\"", "slash\\", "line\n", "tab\t", "\u0000", "\u4e2d\u6587"])",
      R"({"nested":[true,null,{"x":[1,2,"a\r\b\f"]}],"empty":[]})"};
  for (const bool retain_freed : {false, true}) {
    for (const MYSQL_PROTOCOL_TYPE protocol : {TEXT, BINARY}) {
      for (const char *text : texts) {
        SCOPED_TRACE(text);
        ObArenaAllocator input_allocator("JsonCellInput");
        ObObj obj;
        std::string expected;
        ASSERT_EQ(OB_SUCCESS, make_mysql_json_cell_input(text, input_allocator, obj, expected));
        JsonCellScratchAllocator scratch(retain_freed);
        ObMySQLCellValue out;
        ASSERT_EQ(OB_SUCCESS, encode(obj, scratch, out, protocol));
        check_cell(out, expected, scratch);
        ObMySQLCellValue copy(out);
        check_cell(copy, expected, scratch);
        scratch.free(const_cast<char *>(out.get_bytes()));
        EXPECT_EQ(0U, scratch.active_count());
        EXPECT_FALSE(scratch.invalid_free());
      }
    }
  }
}

TEST_F(MysqlJsonCell, multiple_cells_and_buffer_growth_preserve_all_results)
{
  // Control characters expand to six output bytes each. Their binary form is
  // much smaller, forcing the print buffer to grow beyond its initial reserve.
  const std::vector<std::string> texts = {
      "[\"first\"]", "\"" + std::string(64, 'x') + "\"",
      mysql_json_cell_control_text(8192),
      R"({"last":[{"value":"after the large cell"}]})"};
  for (const bool retain_freed : {false, true}) {
    for (const MYSQL_PROTOCOL_TYPE protocol : {TEXT, BINARY}) {
      ObArenaAllocator input_allocator("JsonCellInput");
      std::vector<ObObj> inputs(texts.size());
      std::vector<std::string> expected(texts.size());
      std::vector<ObMySQLCellValue> cells(texts.size());
      JsonCellScratchAllocator scratch(retain_freed);
      for (size_t i = 0; i < texts.size(); ++i) {
        ASSERT_EQ(OB_SUCCESS, make_mysql_json_cell_input(
            texts[i], input_allocator, inputs[i], expected[i]));
        ASSERT_EQ(OB_SUCCESS, encode(inputs[i], scratch, cells[i], protocol));
        for (size_t previous = 0; previous <= i; ++previous) {
          check_cell(cells[previous], expected[previous], scratch);
        }
      }
      EXPECT_GT(scratch.frees(), 0);
      for (ObMySQLCellValue &cell : cells) {
        scratch.free(const_cast<char *>(cell.get_bytes()));
      }
      EXPECT_EQ(0U, scratch.active_count());
      EXPECT_FALSE(scratch.invalid_free());
    }
  }
}

TEST_F(MysqlJsonCell, allocation_failures_release_partial_state_and_keep_previous_cells)
{
  ObArenaAllocator input_allocator("JsonCellInput");
  ObObj obj;
  std::string expected;
  const std::string text = mysql_json_cell_control_text(2048);
  ASSERT_EQ(OB_SUCCESS, make_mysql_json_cell_input(text, input_allocator, obj, expected));
  int64_t allocations = 0;
  {
    JsonCellScratchAllocator scratch;
    ObMySQLCellValue out;
    ASSERT_EQ(OB_SUCCESS, encode(obj, scratch, out));
    allocations = scratch.requests();
    check_cell(out, expected, scratch);
    scratch.free(const_cast<char *>(out.get_bytes()));
    EXPECT_EQ(0U, scratch.active_count());
  }
  ASSERT_GT(allocations, 1);
  for (const bool retain_freed : {false, true}) {
    for (int64_t failure = 0; failure < allocations; ++failure) {
      SCOPED_TRACE(failure);
      JsonCellScratchAllocator scratch(retain_freed);
      ObMySQLCellValue first;
      ASSERT_EQ(OB_SUCCESS, encode(obj, scratch, first));
      const size_t previous_count = scratch.active_count();
      scratch.fail_at(scratch.requests() + failure);
      ObMySQLCellValue failed;
      failed.set_borrowed_bytes(ObMySQLCellValueKind::LENENC_BYTES, "old", 3);
      EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, encode(obj, scratch, failed));
      EXPECT_EQ(1, scratch.failures());
      EXPECT_EQ(ObMySQLCellValueKind::NULL_VALUE, failed.kind_);
      EXPECT_EQ(previous_count, scratch.active_count());
      check_cell(first, expected, scratch);
      // A failed cell must not poison a subsequent serialization in the row.
      scratch.fail_at(-1);
      ObMySQLCellValue recovered;
      ASSERT_EQ(OB_SUCCESS, encode(obj, scratch, recovered));
      check_cell(recovered, expected, scratch);
      check_cell(first, expected, scratch);
      scratch.free(const_cast<char *>(first.get_bytes()));
      scratch.free(const_cast<char *>(recovered.get_bytes()));
      EXPECT_EQ(0U, scratch.active_count());
      EXPECT_FALSE(scratch.invalid_free());
    }
  }
}

TEST_F(MysqlJsonCell, empty_null_pointer_and_truncated_values_keep_the_contract)
{
  for (const MYSQL_PROTOCOL_TYPE protocol : {TEXT, BINARY}) {
    JsonCellScratchAllocator scratch(true);
    ObObj obj;
    ObMySQLCellValue out;
    obj.set_json_value(ObJsonType, static_cast<const char *>(nullptr), 0);
    ASSERT_EQ(OB_SUCCESS, encode(obj, scratch, out, protocol));
    EXPECT_EQ(ObMySQLCellValueKind::LEGACY_LENENC_NULL, out.kind_);
    obj.set_json_value(ObJsonType, static_cast<const char *>(nullptr), 1);
    // ObString normalizes a null backing pointer to an empty value.
    EXPECT_EQ(0, obj.get_string().length());
    EXPECT_EQ(nullptr, obj.get_string().ptr());
    EXPECT_EQ(OB_SUCCESS, encode(obj, scratch, out, protocol));
    EXPECT_EQ(ObMySQLCellValueKind::LEGACY_LENENC_NULL, out.kind_);
    EXPECT_EQ(0, scratch.requests());

    ObArenaAllocator input_allocator("JsonCellInput");
    std::string expected;
    ASSERT_EQ(OB_SUCCESS, make_mysql_json_cell_input("[1,2]", input_allocator, obj, expected));
    const ObString binary = obj.get_string();
    obj.set_json_value(ObJsonType, binary.ptr(), 1);
    EXPECT_NE(OB_SUCCESS, encode(obj, scratch, out, protocol));
    EXPECT_EQ(ObMySQLCellValueKind::NULL_VALUE, out.kind_);
    EXPECT_EQ(0U, scratch.active_count());
    EXPECT_FALSE(scratch.invalid_free());
  }
}

// Call the retained generic traversal without copying its formatting logic.
class JsonArrayPrintOracle : public ObJsonBin
{
public:
  explicit JsonArrayPrintOracle(ObIAllocator *allocator) : ObJsonBin(allocator) {}
  int print_generic(ObJsonBuffer &buffer, const bool pretty, const uint64_t depth = 0) const
  {
    return ObIJsonBase::print_array(buffer, depth + 1, pretty);
  }
  // The unrelated depth() is always-inline but defined only in the production
  // .cpp. Printing uses its explicit depth argument; detect any dependency on it.
  uint32_t depth() const override
  {
    ADD_FAILURE() << "Array-print traversal must not query document depth";
    return 0;
  }
};

std::string json_array_print_bytes(const ObJsonBuffer &buffer)
{
  return buffer.empty() ? std::string() : std::string(buffer.ptr(), buffer.length());
}

void check_json_array_print(ObJsonNode &tree, const ObString &binary, ObIAllocator &allocator)
{
  JsonArrayPrintOracle bin(&allocator);
  ASSERT_EQ(OB_SUCCESS, bin.reset(binary, 0, nullptr));
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, bin.json_type());
  ObILobCursor *const cursor = bin.get_cursor();
  ObJsonBinCtx *const context = bin.get_ctx();
  for (const bool pretty : {false, true}) {
    for (const bool quoted : {false, true}) {
      for (const uint64_t depth : {0U, 3U}) {
        ObJsonBuffer expected(&allocator), generic(&allocator), actual(&allocator);
        ASSERT_EQ(OB_SUCCESS, tree.print(expected, quoted, 0, pretty, depth));
        ASSERT_EQ(OB_SUCCESS, bin.print_generic(generic, pretty, depth));
        ASSERT_EQ(OB_SUCCESS, bin.print(actual, quoted, binary.length(), pretty, depth));
        EXPECT_EQ(json_array_print_bytes(expected), json_array_print_bytes(actual));
        EXPECT_EQ(json_array_print_bytes(generic), json_array_print_bytes(actual));
        EXPECT_EQ(tree.element_count(), bin.element_count());
        EXPECT_EQ(cursor, bin.get_cursor());
        EXPECT_EQ(context, bin.get_ctx());
      }
    }
  }
}

TEST(BinaryJsonArrayPrint, tree_and_generic_traversal_match_complete_nested_and_boundary_outputs)
{
  std::vector<std::string> texts = {
      "[]", "[\"\"]", "[null,true,false,0,-1,255,256,-129,18446744073709551615,1.25,1e100]",
      R"(["quote\"","slash\\","line\n","tab\t","\u0000","\u4e2d\u6587","\ud83d\ude00"])",
      R"([[],{},["",[1,2]],{"a":[true,null,{"x":"a\r\b\f"}],"b":[]}])"};
  for (const int64_t length : {127, 128, 245, 246, 65522, 65523}) {
    texts.push_back("[\"" + std::string(length, 's') + "\",\"\"]");
  }
  for (const int64_t count : {255, 256}) {
    std::string text("[");
    for (int64_t i = 0; i < count; ++i) {
      if (i > 0) { text += ','; }
      text += std::to_string(i);
    }
    texts.push_back(text + ']');
  }
  texts.push_back(std::string(90, '[') + "\"deep\"" + std::string(90, ']'));
  for (const std::string &text : texts) {
    SCOPED_TRACE(text.substr(0, 100));
    ObArenaAllocator allocator("ArrayPrintTree");
    ObJsonNode *tree = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObJsonParser::get_tree(&allocator, text.data(), text.length(), tree));
    for (const bool doc_header : {false, true}) {
      ObString binary;
      ASSERT_EQ(OB_SUCCESS, doc_header ? tree->get_raw_binary(binary, &allocator)
          : tree->get_raw_binary_v0(binary, &allocator));
      check_json_array_print(*tree, binary, allocator);
    }
  }
}

TEST(BinaryJsonArrayPrint, forwarded_values_keep_type_prefix_decoding)
{
  for (const char *text : {R"("forward\u0000tail")", "123456789", "true", "null"}) {
    SCOPED_TRACE(text);
    ObArenaAllocator allocator("ArrayPrintFwd");
    ObJsonNode *value = nullptr;
    ObString value_binary;
    ASSERT_EQ(OB_SUCCESS, ObJsonParser::get_tree(&allocator, text, STRLEN(text), value));
    ASSERT_EQ(OB_SUCCESS, value->get_raw_binary_v0(value_binary, &allocator));
    ObJsonArray tree(&allocator);
    ASSERT_EQ(OB_SUCCESS, tree.append(value));

    // A local array can refer to a value in its document's extension segment.
    ObJsonBinMeta meta;
    meta.set_type(ObJBVerType::J_ARRAY_V0, false);
    meta.set_element_count(1);
    ASSERT_EQ(OB_SUCCESS, meta.calc_entry_array());
    meta.set_obj_size(meta.get_value_entry_offset(1));
    ObJsonBuffer binary(&allocator);
    ASSERT_EQ(OB_SUCCESS, meta.to_header(binary));
    ASSERT_EQ(OB_SUCCESS, ObJsonVar::set_var(0, meta.entry_var_type(),
        binary.ptr() + meta.get_value_entry_offset(0)));
    binary.ptr()[meta.get_value_entry_offset(0) + meta.entry_var_size()] = ObJBVerType::J_FORWARD_V0;
    ASSERT_EQ(OB_SUCCESS, binary.append(value_binary));
    ObJsonBinCtx context;
    context.extend_seg_offset_ = meta.obj_size();
    JsonArrayPrintOracle bin(&allocator);
    ASSERT_EQ(OB_SUCCESS, bin.reset(binary.string(), 0, &context));
    for (const bool pretty : {false, true}) {
      ObJsonBuffer expected(&allocator), generic(&allocator), actual(&allocator);
      ASSERT_EQ(OB_SUCCESS, tree.print(expected, true, 0, pretty));
      ASSERT_EQ(OB_SUCCESS, bin.print_generic(generic, pretty));
      ASSERT_EQ(OB_SUCCESS, bin.print(actual, true, 0, pretty));
      EXPECT_EQ(json_array_print_bytes(expected), json_array_print_bytes(actual));
      EXPECT_EQ(json_array_print_bytes(generic), json_array_print_bytes(actual));
    }
    EXPECT_EQ(&context, bin.get_ctx());
  }
}

TEST(BinaryJsonArrayPrint, child_views_borrow_the_owning_parent_context)
{
  ObArenaAllocator input_allocator("ArrayPrintInput");
  ObJsonNode *tree = nullptr;
  const char *text = R"(["",["nested"],{"a":[false,null]},"last"])";
  ASSERT_EQ(OB_SUCCESS, ObJsonParser::get_tree(&input_allocator, text, STRLEN(text), tree));
  ObString binary;
  ASSERT_EQ(OB_SUCCESS, tree->get_raw_binary(binary, &input_allocator));
  for (const bool retain_freed : {false, true}) {
    JsonCellScratchAllocator allocator(retain_freed);
    {
      JsonArrayPrintOracle bin(&allocator);
      ASSERT_EQ(OB_SUCCESS, bin.reset(binary, 0, nullptr));
      ASSERT_NE(nullptr, bin.get_ctx());
      const size_t parent_allocations = allocator.active_count();
      ASSERT_GT(parent_allocations, 0U);
      for (int repeat = 0; repeat < 3; ++repeat) {
        ObJsonBuffer actual(&input_allocator), expected(&input_allocator);
        ASSERT_EQ(OB_SUCCESS, bin.print(actual, true));
        ASSERT_EQ(OB_SUCCESS, tree->print(expected, true));
        EXPECT_EQ(json_array_print_bytes(expected), json_array_print_bytes(actual));
        EXPECT_EQ(parent_allocations, allocator.active_count());
        EXPECT_FALSE(allocator.invalid_free());
      }
    }
    EXPECT_EQ(0U, allocator.active_count());
    EXPECT_FALSE(allocator.invalid_free());
  }
  // Headerless arrays also support a wrapper without an allocator.
  ASSERT_EQ(OB_SUCCESS, tree->get_raw_binary_v0(binary, &input_allocator));
  JsonArrayPrintOracle bin(nullptr);
  ASSERT_EQ(OB_SUCCESS, bin.reset(binary, 0, nullptr));
  ObJsonBuffer actual(&input_allocator), expected(&input_allocator);
  ASSERT_EQ(OB_SUCCESS, bin.print(actual, true));
  ASSERT_EQ(OB_SUCCESS, tree->print(expected, true));
  EXPECT_EQ(json_array_print_bytes(expected), json_array_print_bytes(actual));
}

TEST(BinaryJsonArrayPrint, accepted_parent_with_damaged_children_matches_generic_errors_and_partial_bytes)
{
  ObArenaAllocator allocator("ArrayPrintBad");
  const char *text = R"(["first",42,"tail"] )";
  ObJsonNode *tree = nullptr;
  ObString binary;
  ASSERT_EQ(OB_SUCCESS, ObJsonParser::get_tree(&allocator, text, STRLEN(text), tree));
  ASSERT_EQ(OB_SUCCESS, tree->get_raw_binary_v0(binary, &allocator));
  int64_t child_errors = 0;
  for (int64_t length = 1; length < binary.length(); ++length) {
    JsonArrayPrintOracle bin(&allocator);
    // Keep the full physical allocation alive, but expose only this prefix.
    if (OB_SUCCESS == bin.reset(ObString(length, binary.ptr()), 0, nullptr)) {
      for (const bool pretty : {false, true}) {
        ObJsonBuffer generic(&allocator), actual(&allocator);
        const int generic_ret = bin.print_generic(generic, pretty);
        const int actual_ret = bin.print(actual, true, 0, pretty);
        EXPECT_EQ(generic_ret, actual_ret) << length;
        EXPECT_EQ(json_array_print_bytes(generic), json_array_print_bytes(actual)) << length;
        child_errors += OB_SUCCESS != actual_ret;
      }
    }
  }
  EXPECT_GT(child_errors, 0);

  ObLobInRowCursor cursor(binary.ptr(), binary.length());
  ObJsonBinMeta meta;
  ObJsonBinMetaParser parser(&cursor, 0, meta);
  ASSERT_EQ(OB_SUCCESS, parser.parse());
  for (const bool invalid_type : {false, true}) {
    std::string damaged(binary.ptr(), binary.length());
    const uint64_t entry = meta.get_value_entry_offset(1);
    if (invalid_type) {
      damaged[entry + meta.entry_var_size()] = ObJBVerType::J_ERROR_V0;
    } else {
      ASSERT_EQ(OB_SUCCESS, ObJsonVar::set_var(binary.length() + 1, meta.entry_var_type(), &damaged[entry]));
      damaged[entry + meta.entry_var_size()] = ObJBVerType::J_STRING_V0;
    }
    JsonArrayPrintOracle bin(&allocator);
    ASSERT_EQ(OB_SUCCESS, bin.reset(ObString(damaged.length(), damaged.data()), 0, nullptr));
    ObJsonBuffer generic(&allocator), actual(&allocator);
    const int generic_ret = bin.print_generic(generic, false);
    EXPECT_NE(OB_SUCCESS, generic_ret);
    EXPECT_EQ(generic_ret, bin.print(actual, true));
    EXPECT_EQ(json_array_print_bytes(generic), json_array_print_bytes(actual));
    EXPECT_EQ(ObJsonNodeType::J_ARRAY, bin.json_type());
    EXPECT_EQ(3U, bin.element_count());
  }
}

class JsonArrayFailingCursor : public ObLobInRowCursor
{
public:
  explicit JsonArrayFailingCursor(const ObString &binary)
    : ObLobInRowCursor(binary.ptr(), binary.length()) {}
  void fail_at(const int64_t read) { reads_ = 0; fail_at_ = read; }
  int64_t reads() const { return reads_; }
  int get(int64_t offset, int64_t length, ObString &data) const override
  {
    return reads_++ == fail_at_ ? OB_ERR_UNEXPECTED : ObLobInRowCursor::get(offset, length, data);
  }
protected:
  int get_ptr(int64_t offset, int64_t length, const char *&ptr) const override
  {
    return reads_++ == fail_at_ ? OB_ERR_UNEXPECTED : ObLobInRowCursor::get_ptr(offset, length, ptr);
  }
private:
  mutable int64_t reads_ = 0;
  int64_t fail_at_ = -1;
};

TEST(BinaryJsonArrayPrint, nonseek_and_external_update_cursors_retain_generic_read_errors)
{
  ObArenaAllocator allocator("ArrayPrCursor");
  const char *text = R"(["one",false,["nested"],{"a":1},""])";
  ObJsonNode *tree = nullptr;
  ObString binary;
  ASSERT_EQ(OB_SUCCESS, ObJsonParser::get_tree(&allocator, text, STRLEN(text), tree));
  ASSERT_EQ(OB_SUCCESS, tree->get_raw_binary_v0(binary, &allocator));
  {
    JsonArrayPrintOracle bin(&allocator);
    ASSERT_EQ(OB_SUCCESS, bin.reset(binary, 0, nullptr));
    bin.set_seek_flag(false);
    ObJsonBuffer generic(&allocator), actual(&allocator), expected(&allocator);
    ASSERT_EQ(OB_SUCCESS, bin.print_generic(generic, true));
    ASSERT_EQ(OB_SUCCESS, bin.print(actual, true, 0, true));
    ASSERT_EQ(OB_SUCCESS, tree->print(expected, true, 0, true));
    EXPECT_EQ(json_array_print_bytes(expected), json_array_print_bytes(actual));
    EXPECT_EQ(json_array_print_bytes(generic), json_array_print_bytes(actual));
    EXPECT_FALSE(bin.get_seek_flag());
  }

  void *cursor_storage = allocator.alloc(sizeof(JsonArrayFailingCursor));
  ASSERT_NE(nullptr, cursor_storage);
  auto *cursor = new (cursor_storage) JsonArrayFailingCursor(binary);
  // The update context destroys its cursor; the arena owns its storage.
  ObJsonBinUpdateCtx update(&allocator);
  update.set_lob_cursor(cursor);
  ObJsonBinCtx context;
  context.update_ctx_ = &update;
  JsonArrayPrintOracle bin(&allocator);
  ASSERT_EQ(OB_SUCCESS, bin.reset(ObString(), 0, &context));
  ASSERT_EQ(cursor, bin.get_cursor());
  for (const bool seek_only : {false, true}) {
    bin.set_seek_flag(seek_only);
    for (const bool pretty : {false, true}) {
      int64_t reads = 0;
      {
        cursor->fail_at(-1);
        ObJsonBuffer generic(&allocator), actual(&allocator), expected(&allocator);
        ASSERT_EQ(OB_SUCCESS, bin.print_generic(generic, pretty));
        reads = cursor->reads();
        cursor->fail_at(-1);
        ASSERT_EQ(OB_SUCCESS, bin.print(actual, true, 0, pretty));
        ASSERT_EQ(OB_SUCCESS, tree->print(expected, true, 0, pretty));
        EXPECT_EQ(json_array_print_bytes(expected), json_array_print_bytes(actual));
        EXPECT_EQ(json_array_print_bytes(generic), json_array_print_bytes(actual));
      }
      ASSERT_GT(reads, 0);
      for (int64_t failure = 0; failure < reads; ++failure) {
        SCOPED_TRACE(failure);
        ObJsonBuffer generic(&allocator), actual(&allocator);
        cursor->fail_at(failure);
        const int generic_ret = bin.print_generic(generic, pretty);
        cursor->fail_at(failure);
        EXPECT_EQ(generic_ret, bin.print(actual, true, 0, pretty));
        EXPECT_EQ(json_array_print_bytes(generic), json_array_print_bytes(actual));
        EXPECT_EQ(cursor, bin.get_cursor());
        EXPECT_EQ(&context, bin.get_ctx());
      }
    }
  }
  cursor->fail_at(-1);
  ObJsonBuffer recovered(&allocator), expected(&allocator);
  ASSERT_EQ(OB_SUCCESS, bin.print(recovered, true));
  ASSERT_EQ(OB_SUCCESS, tree->print(expected, true));
  EXPECT_EQ(json_array_print_bytes(expected), json_array_print_bytes(recovered));
}

TEST(BinaryJsonArrayPrint, every_output_allocation_failure_keeps_parent_usable)
{
  ObArenaAllocator allocator("ArrayPrintFail");
  const std::string text = "[\"first\",[" + mysql_json_cell_control_text(2048) + "],\"last\"]";
  ObJsonNode *tree = nullptr;
  ObString binary;
  ASSERT_EQ(OB_SUCCESS, ObJsonParser::get_tree(&allocator, text.data(), text.length(), tree));
  ASSERT_EQ(OB_SUCCESS, tree->get_raw_binary(binary, &allocator));
  JsonArrayPrintOracle bin(&allocator);
  ASSERT_EQ(OB_SUCCESS, bin.reset(binary, 0, nullptr));
  ObJsonBuffer expected(&allocator);
  ASSERT_EQ(OB_SUCCESS, tree->print(expected, true));
  int64_t allocations = 0;
  {
    JsonCellScratchAllocator scratch;
    {
      ObJsonBuffer actual(&scratch);
      ASSERT_EQ(OB_SUCCESS, bin.print(actual, true));
      EXPECT_EQ(json_array_print_bytes(expected), json_array_print_bytes(actual));
      allocations = scratch.requests();
    }
    EXPECT_EQ(0U, scratch.active_count());
  }
  ASSERT_GT(allocations, 1);
  for (const bool retain_freed : {false, true}) {
    for (int64_t failure = 0; failure < allocations; ++failure) {
      SCOPED_TRACE(failure);
      JsonCellScratchAllocator actual_allocator(retain_freed), generic_allocator(retain_freed);
      actual_allocator.fail_at(failure);
      generic_allocator.fail_at(failure);
      {
        ObJsonBuffer actual(&actual_allocator), generic(&generic_allocator);
        EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, bin.print(actual, true));
        EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, bin.print_generic(generic, false));
        EXPECT_EQ(json_array_print_bytes(generic), json_array_print_bytes(actual));
        EXPECT_EQ(1, actual_allocator.failures());
        EXPECT_EQ(1, generic_allocator.failures());
      }
      EXPECT_EQ(0U, actual_allocator.active_count());
      EXPECT_EQ(0U, generic_allocator.active_count());
      EXPECT_FALSE(actual_allocator.invalid_free());
      EXPECT_FALSE(generic_allocator.invalid_free());
      ObJsonBuffer recovered(&allocator);
      ASSERT_EQ(OB_SUCCESS, bin.print(recovered, true));
      EXPECT_EQ(json_array_print_bytes(expected), json_array_print_bytes(recovered));
    }
  }
}
} // namespace
} // namespace sql
} // namespace oceanbase
