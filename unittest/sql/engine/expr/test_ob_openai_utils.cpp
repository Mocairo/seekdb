#include "ai_test_support.h"
#include <string>

using namespace oceanbase::common;

int main()
{
  run_ai_prompt_tests();
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObIJsonBase *output = nullptr;
  AI_CHECK(OB_SUCCESS == embed.parse_output(allocator,
      ai_test_json(allocator, R"({"data":[{"index":1,"embedding":[3,4]},{"index":0,"embedding":[1,2]}]})"), output));
  ObString serialized;
  AI_CHECK(OB_SUCCESS == ObAIFuncJsonUtils::print_json_to_str(allocator, output, serialized));
  AI_CHECK(std::string(serialized.ptr(), serialized.length()) == "[[1, 2], [3, 4]]");
  const char *invalid[] = {
    R"({"data":{}})", R"({"data":[]})",
    R"({"data":[{"index":0,"embedding":[]}]})",
    R"({"data":[{"index":0,"embedding":[true]}]})",
    R"({"data":[{"index":0,"embedding":[null]}]})",
    R"({"data":[{"index":0,"embedding":["1"]}]})",
    R"({"data":[{"index":-1,"embedding":[1]}]})",
    R"({"data":[{"index":1,"embedding":[1]}]})",
    R"({"data":[{"index":"0","embedding":[1]}]})",
    R"({"data":[{"embedding":[1]}]})",
    R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})",
    R"({"data":[{"index":0,"embedding":[1]},{"index":1,"embedding":[2,3]}]})",
  };
  for (const char *text : invalid) {
    AI_CHECK(OB_SUCCESS != embed.parse_output(allocator, ai_test_json(allocator, text), output));
  }
  ObOllamaUtils::ObOllamaComplete complete;
  ObArray<ObString> headers;
  ObString key;
  AI_CHECK(OB_SUCCESS == complete.get_header(allocator, key, headers));
  AI_CHECK(headers.count() == 1 && headers.at(0) == ObString("Content-Type: application/json"));
  ObString model("arbitrary-model"), system, prompt("arbitrary prompt");
  ObJsonObject *body = nullptr;
  AI_CHECK(OB_SUCCESS == complete.get_body(allocator, model, system, prompt, nullptr, body));
  AI_CHECK(body->get_value("model")->get_data_length() == model.length());
  AI_CHECK(body->get_value("stream")->json_type() == ObJsonNodeType::J_BOOLEAN);
  AI_CHECK(!body->get_value("stream")->get_boolean());
  AI_CHECK(OB_SUCCESS == complete.parse_output(allocator,
      ai_test_json(allocator, R"({"response":"ok"})"), output));
  AI_CHECK(OB_SUCCESS != complete.parse_output(allocator,
      ai_test_json(allocator, R"({"response":""})"), output));
  ObOllamaUtils::ObOllamaEmbed ollama_embed;
  AI_CHECK(OB_SUCCESS == ollama_embed.parse_output(allocator,
      ai_test_json(allocator, R"({"embeddings":[[1,2],[3,4]]})"), output));
  AI_CHECK(OB_SUCCESS != ollama_embed.parse_output(allocator,
      ai_test_json(allocator, R"({"embeddings":[[1,2],[3]]})"), output));
  AI_CHECK(OB_SUCCESS != ollama_embed.parse_output(allocator,
      ai_test_json(allocator, R"({"embeddings":[[null]]})"), output));
  std::puts("Provider parsing, ordering, vector validation and Ollama protocol: passed");
  return 0;
}
