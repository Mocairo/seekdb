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

#include <gtest/gtest.h>
#include <string>
#include "common/json_type/ob_json_parse.h"
#include "common/json_type/ob_json_tree.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "sql/engine/expr/ob_expr_operator.h"
#define private public
#include "sql/engine/expr/ob_expr_tokenize.h"
#undef private

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace storage;

namespace
{
int read_rebuilt_tokenize_properties(const ObString &parser_name,
                                    const ObString &json,
                                    ObFTParserProperty &property)
{
  int ret = OB_SUCCESS;
  ObFTParserJsonProps props;
  ObFTParser parser;
  if (OB_FAIL(props.init())) {
  } else if (OB_FAIL(props.parse_from_valid_str(json))) {
  } else if (OB_FAIL(props.rebuild_props_for_ddl(parser_name, CS_TYPE_UTF8MB4_BIN, false))) {
  } else if (OB_FAIL(parser.parse_from_str(parser_name.ptr(), parser_name.length()))) {
  } else if (OB_FAIL(property.parse_for_parser_helper(parser, props))) {
  }
  return ret;
}

void expect_parser_predicates_match_legacy(const ObFTParser &parser)
{
  const bool actual[] = {parser.is_space(), parser.is_ngram(), parser.is_beng(),
                          parser.is_ik(), parser.is_ngram2()};
  bool expected_builtin = false;
  for (int64_t type = 0; type < ObFTParser::FTP_MAX; ++type) {
    const bool expected = ObFTParserName(ObFTParser::NAME_STR[type]) == parser.get_parser_name();
    EXPECT_EQ(expected, actual[type]) << ObFTParser::NAME_STR[type];
    expected_builtin = expected_builtin || expected;
  }
  EXPECT_EQ(expected_builtin, parser.is_builtin());
  EXPECT_EQ(expected_builtin && parser.get_parser_version() >= 0, parser.is_valid());
  EXPECT_EQ(actual[ObFTParser::FTP_SPACE] || actual[ObFTParser::FTP_NGRAM]
                || actual[ObFTParser::FTP_BENG], parser.is_type_before_4_3_5_1());
}
} // namespace

TEST(FTParserProperties, canonical_predicates_preserve_normalization_and_serialized_versions)
{
  for (int64_t type = 0; type < ObFTParser::FTP_MAX; ++type) {
    const std::string canonical(ObFTParser::NAME_STR[type]);
    for (size_t case_mask = 0; case_mask < (size_t(1) << canonical.size()); ++case_mask) {
      std::string input(canonical);
      for (size_t i = 0; i < input.size(); ++i) {
        if ((case_mask & (size_t(1) << i)) && input[i] >= 'a' && input[i] <= 'z') {
          input[i] -= 'a' - 'A';
        }
      }
      SCOPED_TRACE(input);
      ObFTParser parser;
      ASSERT_EQ(OB_SUCCESS, parser.init(ObString(input.size(), input.data())));
      EXPECT_EQ(canonical, parser.get_parser_name().str());
      EXPECT_EQ(int64_t(ObFTParser::BUILTIN_VERSION), parser.get_parser_version());
      expect_parser_predicates_match_legacy(parser);

      const std::string serialized_input = input + ".17";
      ObFTParser restored;
      ASSERT_EQ(OB_SUCCESS, restored.parse_from_str(serialized_input.data(), serialized_input.size()));
      EXPECT_EQ(17, restored.get_parser_version());
      expect_parser_predicates_match_legacy(restored);
      char output[OB_FT_PARSER_NAME_LENGTH];
      ASSERT_EQ(OB_SUCCESS, restored.serialize_to_str(output, sizeof(output)));
      EXPECT_EQ(canonical + ".17", output);

      ObFTParser copied;
      copied.set_name_and_version(ObFTParserName(input.c_str()), 0);
      EXPECT_EQ(canonical, copied.get_parser_name().str());
      expect_parser_predicates_match_legacy(copied);
    }
  }
}

TEST(FTParserProperties, canonical_predicates_preserve_unknown_names_and_stored_bytes)
{
  ObFTParser uninitialized;
  expect_parser_predicates_match_legacy(uninitialized);
  EXPECT_FALSE(uninitialized.is_builtin());
  for (const char *input : {"default", "english", "basic_english", "whitespace", "beng_extra",
                             "ngram20", "ngra", "ik.1", "space ", " beng"}) {
    SCOPED_TRACE(input);
    ObFTParser unknown;
    EXPECT_EQ(OB_FUNCTION_NOT_DEFINED, unknown.init(ObString(input)));
    expect_parser_predicates_match_legacy(unknown);
    EXPECT_FALSE(unknown.is_builtin());
  }
  for (const int64_t version : {int64_t(-1), int64_t(0), int64_t(1), int64_t(17)}) {
    ObFTParser parser;
    parser.set_name_and_version(ObFTParserName("BENG"), version);
    expect_parser_predicates_match_legacy(parser);
    EXPECT_TRUE(parser.is_beng());
    EXPECT_EQ(version >= 0, parser.is_valid());
  }
  // The public mutable name buffer can bypass normalization. Predicates must
  // compare the stored bytes exactly, just like the original equality call.
  ObFTParserName raw_name("beng");
  raw_name.str()[0] = 'B';
  ObFTParser raw;
  raw.set_name_and_version(raw_name, 1);
  expect_parser_predicates_match_legacy(raw);
  EXPECT_FALSE(raw.is_builtin());
  const std::string unknown_serialized("unknown.1");
  EXPECT_EQ(OB_FUNCTION_NOT_DEFINED, raw.parse_from_str(
      unknown_serialized.data(), unknown_serialized.size()));
  expect_parser_predicates_match_legacy(raw);
}

TEST(FTParserProperties, canonical_predicates_preserve_length_boundaries_and_embedded_nul)
{
  for (const size_t length : {size_t(63), size_t(64), size_t(65)}) {
    std::string input(length, 'x');
    ObFTParser parser;
    EXPECT_EQ(length < OB_FT_PARSER_NAME_LENGTH ? OB_FUNCTION_NOT_DEFINED : OB_INVALID_ARGUMENT,
        parser.init(ObString(input.size(), input.data())));
    expect_parser_predicates_match_legacy(parser);
    EXPECT_FALSE(parser.is_builtin());

    input.replace(0, 4, "BeNg");
    input[4] = '\0';
    ObFTParser nul_name;
    EXPECT_EQ(length < OB_FT_PARSER_NAME_LENGTH ? OB_SUCCESS : OB_INVALID_ARGUMENT,
        nul_name.init(ObString(input.size(), input.data())));
    expect_parser_predicates_match_legacy(nul_name);
    EXPECT_EQ(length < OB_FT_PARSER_NAME_LENGTH, nul_name.is_beng());
  }
  const std::string nul_input("Ik\0unknown", 10);
  ObFTParser parser;
  ASSERT_EQ(OB_SUCCESS, parser.init(ObString(nul_input.size(), nul_input.data())));
  EXPECT_TRUE(parser.is_ik());
  expect_parser_predicates_match_legacy(parser);

  // Failed replacement retains the old name/version, as before this change.
  const std::string oversized(OB_FT_PARSER_NAME_LENGTH, 'x');
  EXPECT_EQ(OB_INVALID_ARGUMENT, parser.init(ObString(oversized.size(), oversized.data())));
  EXPECT_TRUE(parser.is_ik());
  EXPECT_EQ(int64_t(ObFTParser::BUILTIN_VERSION), parser.get_parser_version());
  expect_parser_predicates_match_legacy(parser);
  ObFTParser empty;
  EXPECT_EQ(OB_INVALID_ARGUMENT, empty.init(ObString()));
  expect_parser_predicates_match_legacy(empty);
}

TEST(FTParserProperties, tokenize_defaults_match_rebuilt_properties_for_every_parser)
{
  ObArenaAllocator allocator("FTPropTest");
  for (const char *name : {"space", "beng", "ngram", "ngram2", "ik"}) {
    SCOPED_TRACE(name);
    ObExprTokenize::TokenizeParam param(allocator);
    ASSERT_EQ(OB_SUCCESS, ObExprTokenize::construct_ft_parser_inner_name(ObString(name), param));
    ObFTParserProperty expected;
    ASSERT_EQ(OB_SUCCESS, read_rebuilt_tokenize_properties(param.parser_name_, ObString(), expected));
    ASSERT_EQ(OB_SUCCESS, param.reform_parser_properties(ObString()));
    EXPECT_TRUE(param.parsed_properties_.is_equal(expected));
    // An explicit empty additional_args array becomes "{}" and uses validation.
    ASSERT_EQ(OB_SUCCESS, param.reform_parser_properties(ObString("{}")));
    EXPECT_TRUE(param.parsed_properties_.is_equal(expected));
  }
}

TEST(FTParserProperties, tokenize_custom_and_invalid_properties_do_not_use_defaults)
{
  struct PropertyCase
  {
    const char *parser_;
    const char *custom_;
    const char *invalid_;
  };
  const PropertyCase cases[] = {
    {"space", "{\"min_token_size\":5,\"max_token_size\":10}", "{\"min_token_size\":0}"},
    {"beng", "{\"min_token_size\":6,\"max_token_size\":12}", "{\"max_token_size\":9}"},
    {"ngram", "{\"ngram_token_size\":1}", "{\"ngram_token_size\":0}"},
    {"ngram2", "{\"min_ngram_size\":1,\"max_ngram_size\":4}",
        "{\"min_ngram_size\":4,\"max_ngram_size\":3}"},
    {"ik", "{\"ik_mode\":\"max_word\"}", "{\"ik_mode\":\"invalid\"}"},
  };
  ObArenaAllocator allocator("FTPropTest");
  for (const PropertyCase &test : cases) {
    SCOPED_TRACE(test.parser_);
    ObExprTokenize::TokenizeParam param(allocator);
    ASSERT_EQ(OB_SUCCESS, ObExprTokenize::construct_ft_parser_inner_name(ObString(test.parser_), param));
    ObFTParserProperty custom;
    ASSERT_EQ(OB_SUCCESS, read_rebuilt_tokenize_properties(param.parser_name_, ObString(test.custom_), custom));
    ASSERT_EQ(OB_SUCCESS, param.reform_parser_properties(ObString(test.custom_)));
    EXPECT_TRUE(param.parsed_properties_.is_equal(custom));
    ObFTParserProperty defaults;
    ASSERT_EQ(OB_SUCCESS, read_rebuilt_tokenize_properties(param.parser_name_, ObString(), defaults));
    ASSERT_EQ(OB_SUCCESS, param.reform_parser_properties(ObString()));
    EXPECT_TRUE(param.parsed_properties_.is_equal(defaults));
    EXPECT_FALSE(param.parsed_properties_.is_equal(custom));
    EXPECT_EQ(OB_INVALID_ARGUMENT, param.reform_parser_properties(ObString(test.invalid_)));
    EXPECT_EQ(OB_NOT_SUPPORTED, param.reform_parser_properties(ObString("{\"unsupported\":1}")));
    ObFTParserProperty unused;
    const ObString malformed("{]");
    const int expected_error = read_rebuilt_tokenize_properties(param.parser_name_, malformed, unused);
    EXPECT_NE(OB_SUCCESS, expected_error);
    EXPECT_EQ(expected_error, param.reform_parser_properties(malformed));
  }
  ObExprTokenize::TokenizeParam param(allocator);
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObExprTokenize::construct_ft_parser_inner_name(ObString(), param));
  EXPECT_EQ(OB_FUNCTION_NOT_DEFINED,
      ObExprTokenize::construct_ft_parser_inner_name(ObString("unknown"), param));
  param.parser_name_ = ObString();
  EXPECT_EQ(OB_INVALID_ARGUMENT, param.reform_parser_properties(ObString()));
  param.parser_name_ = ObString("unknown.1");
  EXPECT_EQ(OB_FUNCTION_NOT_DEFINED, param.reform_parser_properties(ObString()));
}

TEST(FTParserProperties, tokenize_parameters_and_json_share_the_callers_arena)
{
  ObArenaAllocator allocator("FTPropTest");
  ObString parser_name;
  ObString properties;
  ObIJsonBase *result = nullptr;
  {
    ObExprTokenize::TokenizeParam param(allocator);
    EXPECT_EQ(&allocator, &param.allocator_);
    ASSERT_EQ(OB_SUCCESS, ObExprTokenize::construct_ft_parser_inner_name(ObString("ngram"), param));
    {
      ObArenaAllocator source_allocator("FTPropSource");
      ObJsonNode *source = nullptr;
      ASSERT_EQ(OB_SUCCESS, ObJsonParser::get_tree(&source_allocator,
          ObString("{\"additional_args\":[{\"ngram_token_size\":1}]}"), source));
      ASSERT_EQ(OB_SUCCESS, param.parse_json_param(source));
    }
    // The parsed option tree is gone before validation and tokenization.
    ASSERT_EQ(OB_SUCCESS, param.reform_parser_properties(param.properties_));
    param.meta_.set_varchar();
    param.meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    param.fulltext_ = ObString("aaaa");
    param.output_mode_ = ObExprTokenize::TokenizeParam::ALL;
    ASSERT_EQ(OB_SUCCESS, ObExprTokenize::tokenize_fulltext(param, param.output_mode_, allocator, result));
    parser_name = param.parser_name_;
    properties = param.properties_;
  }
  // Destroying parameters must not reset the caller's arena or its JSON result.
  EXPECT_EQ(ObString("ngram.1"), parser_name);
  EXPECT_EQ(ObString("{\"ngram_token_size\": 1}"), properties);
  ObJsonBuffer buffer(&allocator);
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(OB_SUCCESS, result->print(buffer, false));
  EXPECT_EQ(ObString("{\"tokens\": [{\"a\": 4}], \"doc_len\": 4}"), buffer.string());
}

TEST(FTParserProperties, rebuilt_tree_matches_serialized_helper_properties)
{
  struct PropertyCase
  {
    const char *parser_;
    const char *json_;
  };
  const PropertyCase cases[] = {
    {"space.1", ""},
    {"beng.1", ""},
    {"ngram.1", ""},
    {"ngram2.1", ""},
    {"ik.1", ""},
    {"space.1", "{\"min_token_size\":5,\"max_token_size\":10}"},
    {"beng.1", "{\"min_token_size\":6,\"max_token_size\":12}"},
    {"ngram.1", "{\"ngram_token_size\":1}"},
    {"ngram.1", "{\"ngram_token_size\":10}"},
    {"ngram2.1", "{\"min_ngram_size\":1,\"max_ngram_size\":3}"},
    {"ik.1", "{\"ik_mode\":\"max_word\"}"},
    {"ik.1", "{\"ik_mode\":\"SMART\"}"},
  };
  ObArenaAllocator allocator("FTPropTest");
  for (const PropertyCase &test : cases) {
    SCOPED_TRACE(test.parser_);
    SCOPED_TRACE(test.json_);
    ObFTParser parser;
    ObFTParserProperty direct;
    ObFTParserProperty serialized;
    ObString json;
    ASSERT_EQ(OB_SUCCESS, parser.parse_from_str(test.parser_, STRLEN(test.parser_)));
    {
      ObFTParserJsonProps props;
      ASSERT_EQ(OB_SUCCESS, props.init());
      ASSERT_EQ(OB_SUCCESS, props.parse_from_valid_str(ObString(test.json_)));
      ASSERT_EQ(OB_SUCCESS, props.rebuild_props_for_ddl(
          ObString(test.parser_), CS_TYPE_UTF8MB4_BIN, false));
      ASSERT_EQ(OB_SUCCESS, direct.parse_for_parser_helper(parser, props));
      ASSERT_EQ(OB_SUCCESS, props.to_format_json(allocator, json));
      ASSERT_EQ(OB_SUCCESS, serialized.parse_for_parser_helper(parser, json));
      EXPECT_TRUE(direct.is_equal(serialized));
    }
    // The helper must remain valid after the source JSON tree has been freed.
    ObFTParseHelper helper;
    ASSERT_EQ(OB_SUCCESS, helper.init(&allocator, ObString(test.parser_), direct));
    bool same = false;
    ASSERT_EQ(OB_SUCCESS, helper.check_is_the_same(ObString(test.parser_), json, same));
    EXPECT_TRUE(same);
    EXPECT_EQ(OB_INIT_TWICE, helper.init(&allocator, ObString(test.parser_), direct));
    helper.reset();
    EXPECT_EQ(OB_SUCCESS, helper.init(&allocator, ObString(test.parser_), direct));
  }
}

TEST(FTParserProperties, preserve_missing_and_invalid_properties)
{
  ObFTParser parser;
  ObFTParserProperty property;
  ASSERT_EQ(OB_SUCCESS, parser.init(ObString("ngram")));
  EXPECT_EQ(OB_SUCCESS, property.parse_for_parser_helper(parser, ObString()));
  EXPECT_EQ(ObFTSLiteral::FT_DEFAULT_NGRAM_TOKEN_SIZE, property.ngram_token_size_);
  // An explicitly supplied object with a missing property has always been rejected.
  EXPECT_EQ(OB_SEARCH_NOT_FOUND, property.parse_for_parser_helper(parser, ObString("{}")));

  ObFTParserJsonProps props;
  ASSERT_EQ(OB_SUCCESS, props.init());
  ASSERT_EQ(OB_SUCCESS, props.parse_from_valid_str(ObString("{\"ngram_token_size\":0}")));
  EXPECT_EQ(OB_INVALID_ARGUMENT, props.rebuild_props_for_ddl(
      ObString("ngram.1"), CS_TYPE_UTF8MB4_BIN, false));

  ObFTParserJsonProps unsupported;
  ASSERT_EQ(OB_SUCCESS, unsupported.init());
  ASSERT_EQ(OB_SUCCESS, unsupported.parse_from_valid_str(ObString("{\"ngram_token_size\":2}")));
  EXPECT_EQ(OB_NOT_SUPPORTED, unsupported.rebuild_props_for_ddl(
      ObString("ik.1"), CS_TYPE_UTF8MB4_BIN, false));
}

TEST(FTParserProperties, typed_helper_preserves_tokens_frequencies_and_collation)
{
  ObArenaAllocator allocator("FTPropTest");
  ObFTParser parser;
  ObFTParserJsonProps props;
  ObFTParserProperty property;
  ObString json;
  ASSERT_EQ(OB_SUCCESS, parser.init(ObString("ngram")));
  ASSERT_EQ(OB_SUCCESS, props.init());
  ASSERT_EQ(OB_SUCCESS, props.config_set_ngram_token_size(1));
  ASSERT_EQ(OB_SUCCESS, props.rebuild_props_for_ddl(ObString("ngram.1"), CS_TYPE_UTF8MB4_BIN, false));
  ASSERT_EQ(OB_SUCCESS, property.parse_for_parser_helper(parser, props));
  ASSERT_EQ(OB_SUCCESS, props.to_format_json(allocator, json));
  ObFTParseHelper direct_helper;
  ObFTParseHelper serialized_helper;
  ASSERT_EQ(OB_SUCCESS, direct_helper.init(&allocator, ObString("ngram.1"), property));
  ASSERT_EQ(OB_SUCCESS, serialized_helper.init(&allocator, ObString("ngram.1"), json));
  for (ObCollationType collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI}) {
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(collation);
    const ObString text("Aa\xc3\xa1\xe4\xb8\xad\xe6\x96\x87" "A");
    ObFTWordMap direct_words;
    ObFTWordMap serialized_words;
    ASSERT_EQ(OB_SUCCESS, direct_words.create(17, "FTPropTest"));
    ASSERT_EQ(OB_SUCCESS, serialized_words.create(17, "FTPropTest"));
    int64_t direct_length = 0;
    int64_t serialized_length = 0;
    ASSERT_EQ(OB_SUCCESS, direct_helper.segment(meta, text.ptr(), text.length(), direct_length, direct_words));
    ASSERT_EQ(OB_SUCCESS, serialized_helper.segment(meta, text.ptr(), text.length(), serialized_length, serialized_words));
    EXPECT_EQ(6, direct_length);
    EXPECT_EQ(serialized_length, direct_length);
    EXPECT_EQ(serialized_words.size(), direct_words.size());
    for (auto it = direct_words.begin(); it != direct_words.end(); ++it) {
      int64_t count = 0;
      ASSERT_EQ(OB_SUCCESS, serialized_words.get_refactored(it->first, count));
      EXPECT_EQ(count, it->second);
    }
    ObIJsonBase *direct_json = nullptr;
    ObIJsonBase *serialized_json = nullptr;
    ASSERT_EQ(OB_SUCCESS, direct_helper.make_detail_json(direct_words, direct_length, direct_json));
    ASSERT_EQ(OB_SUCCESS, serialized_helper.make_detail_json(serialized_words, serialized_length, serialized_json));
    ObJsonBuffer direct_buffer(&allocator);
    ObJsonBuffer serialized_buffer(&allocator);
    ASSERT_EQ(OB_SUCCESS, direct_json->print(direct_buffer, false));
    ASSERT_EQ(OB_SUCCESS, serialized_json->print(serialized_buffer, false));
    EXPECT_EQ(serialized_buffer.string(), direct_buffer.string());
    direct_words.destroy();
    serialized_words.destroy();
  }
}
} // namespace sql
} // namespace oceanbase
