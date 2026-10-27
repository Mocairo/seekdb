#include "ai_test_support.h"
#include <string>

using namespace oceanbase::common;

void run_ai_prompt_tests()
{
  ObArenaAllocator allocator;
  struct Case { const char *json; const char *expected; };
  const Case cases[] = {
    {R"({"template":"{0}/{0}/{1}","args":["long repeated string","x"]})",
     "long repeated string/long repeated string/x"},
    {R"({"template":"{0}","args":[[1,true,{"k":2}]]})", "[1, true, {\"k\": 2}]"},
    {R"({"template":"{0}","args":[{"k":2}]})", "{\"k\": 2}"},
    {R"({"template":"outer {0}","args":[{"template":"inner {0}","args":["value"]}]})",
     "outer inner value"},
    {R"({"template":"{0}","args":["\u4f60\u597d\n\"\\"]})", "\xe4\xbd\xa0\xe5\xa5\xbd\n\"\\"},
    {R"({"template":"literal {text} {0}","args":[""]})", "literal {text} "},
  };
  for (const auto &item : cases) {
    ObString output;
    AI_CHECK(OB_SUCCESS == ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
        allocator, ai_test_json(allocator, item.json), output));
    AI_CHECK(std::string(output.ptr(), output.length()) == item.expected);
  }
  const char *invalid[] = {
    R"({"template":"{1}","args":["x"]})",
    R"({"template":"{9999999999999999999999999999999}","args":["x"]})",
    R"({"template":1,"args":[]})",
    R"({"template":"x","args":{}})",
  };
  for (const char *text : invalid) {
    ObString output;
    AI_CHECK(OB_SUCCESS != ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
        allocator, ai_test_json(allocator, text), output));
  }
  std::puts("AI_PROMPT: 10 C++ cases passed");
}
