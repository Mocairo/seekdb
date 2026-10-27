add_executable(test_file_scan_utils
  ${PROJECT_SOURCE_DIR}/unittest/all_tests_main.cpp
  ${PROJECT_SOURCE_DIR}/unittest/sql/engine/basic/test_file_scan_utils.cpp
  ${PROJECT_SOURCE_DIR}/src/sql/engine/basic/ob_file_scan_utils.cpp)
target_compile_features(test_file_scan_utils PRIVATE cxx_std_20)
target_compile_definitions(test_file_scan_utils PRIVATE
  FILE_SQL_STANDALONE_TEST FILE_SQL_SCANNER_ONLY)
target_include_directories(test_file_scan_utils PRIVATE ${PROJECT_SOURCE_DIR}/src)
if(MSVC)
  target_compile_options(test_file_scan_utils PRIVATE /EHsc)
endif()
add_test(NAME file_scan_utils COMMAND test_file_scan_utils
  ${CMAKE_BINARY_DIR}/file-sql-data)
set_tests_properties(file_scan_utils PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_executable(test_file_sql_parser
  ${PROJECT_SOURCE_DIR}/unittest/all_tests_main.cpp
  ${PROJECT_SOURCE_DIR}/unittest/sql/parser/test_file_sql_parser.cpp)
target_compile_definitions(test_file_sql_parser PRIVATE
  FILE_SQL_STANDALONE_TEST FILE_SQL_PARSER_ONLY)
target_link_libraries(test_file_sql_parser PRIVATE
  oceanbase_static ob_sql_static ob_storage_static ob_share_static oblib
  ob_malloc sql_nio $<$<PLATFORM_ID:Windows>:synchronization>)
add_test(NAME file_sql_parser COMMAND test_file_sql_parser)
if(WIN32)
  target_link_libraries(test_file_sql_parser PRIVATE
    ${CMAKE_BINARY_DIR}/src/oblib/lib/compress/zstd_1_3_8/zstd_1_3_8_objs.lib)
  set_tests_properties(file_sql_parser PROPERTIES ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:${OB_VCPKG_DIR}/bin")
endif()
if(UNIX)
  add_test(NAME file_sql_shutdown_reporting
    COMMAND "${Python3_EXECUTABLE}" -B
      "${PROJECT_SOURCE_DIR}/unittest/sql/test_file_sql_shutdown.py")
endif()
