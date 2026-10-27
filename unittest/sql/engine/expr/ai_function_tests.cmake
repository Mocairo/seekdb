add_executable(test_ai_function_utils
  ${PROJECT_SOURCE_DIR}/unittest/all_tests_main.cpp
  ${PROJECT_SOURCE_DIR}/unittest/sql/engine/expr/ob_expr_ai_prompt_test.cpp
  ${PROJECT_SOURCE_DIR}/unittest/sql/engine/expr/test_ob_openai_utils.cpp)
target_compile_definitions(test_ai_function_utils PRIVATE
  AI_FUNCTION_STANDALONE_TEST AI_FUNCTION_TESTS_ONLY)
target_link_libraries(test_ai_function_utils PRIVATE
  oceanbase_static ob_sql_static ob_storage_static ob_share_static oblib
  ob_malloc sql_nio $<$<PLATFORM_ID:Windows>:synchronization>)
add_test(NAME ai_function_utils COMMAND test_ai_function_utils)
if(WIN32)
  target_link_libraries(test_ai_function_utils PRIVATE
    ${CMAKE_BINARY_DIR}/src/oblib/lib/compress/zstd_1_3_8/zstd_1_3_8_objs.lib)
  target_compile_options(test_ai_function_utils PRIVATE -Wno-nontrivial-memcall)
  set_tests_properties(ai_function_utils PROPERTIES ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:${OB_VCPKG_DIR}/bin")
endif()
