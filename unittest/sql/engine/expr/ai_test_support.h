#pragma once

#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include <cstdio>
#include <cstdlib>

#define AI_CHECK(condition) do { \
  if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    std::exit(1); \
  } \
} while (false)

inline oceanbase::common::ObJsonObject *ai_test_json(
    oceanbase::common::ObIAllocator &allocator, const char *text)
{
  using namespace oceanbase::common;
  ObString input(text);
  ObJsonObject *object = nullptr;
  AI_CHECK(OB_SUCCESS == ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, object));
  return object;
}

void run_ai_prompt_tests();
