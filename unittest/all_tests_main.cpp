/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#if defined(FILE_SQL_SCANNER_ONLY)
int run_file_scan_tests(int argc, char **argv);
int main(int argc, char **argv) { return run_file_scan_tests(argc, argv); }
#elif defined(FILE_SQL_PARSER_ONLY)
int run_file_sql_parser_tests();
int main() { return run_file_sql_parser_tests(); }
#elif defined(AI_FUNCTION_TESTS_ONLY)
int run_ai_function_tests();
int main() { return run_ai_function_tests(); }
#else
#include <gtest/gtest.h>
int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
