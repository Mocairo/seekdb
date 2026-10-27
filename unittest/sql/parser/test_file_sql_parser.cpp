/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#include <iostream>
#include <stdexcept>

#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

int run_file_sql_parser_tests() {
  int count = 0;
  const char *queries[] = {
      "select * from read_csv('/allowed/a.csv')",
      "select f.id as v from "
      "read_csv('/allowed/a.csv',header=true,sample_size=-1) as f",
      "select * from "
      "read_csv('/allowed/"
      "a.csv',delim=';',quote='\"',escape='\"',nullstr='NA') f",
      "select * from read_json('/allowed/a.jsonl',format='newline_delimited')",
      "select * from read_ndjson('/allowed/a.jsonl') j",
      "select c1 from read_parquet('/allowed/a.parquet') p",
      "select file from glob('/allowed/*')",
      "describe select * from read_csv('/allowed/a.csv')",
      "desc select id as named_column,count(*) as total from "
      "read_csv('/allowed/a.csv') group by id",
      "explain select * from read_csv('/allowed/a.csv')",
      "select a.id,b.name from read_csv('/allowed/a.csv') a left join "
      "read_json('/allowed/b.jsonl') b on a.id=b.id",
      "copy (select * from read_csv('/allowed/a.csv') order by id) to "
      "'/allowed/out.csv' (format csv,header,nullstr 'NULL')",
      "copy ordinary_table to '/allowed/out.csv' (header false,delimiter "
      "';',quote '\"',escape '\"')",
      "copy (select 1 as v union all select 2) to '/allowed/out.csv' (header)",
      "select 1+2 as ordinary_sql",
      "select * from ordinary_table where id>1 order by id limit 3",
      "describe ordinary_table",
      "explain select * from ordinary_table",
      "copy (select id,id+10 as id from read_csv('/allowed/a.csv') order by 1 desc) "
      "to '/allowed/out.csv' (header)",
      "copy (select 1 as x,2 as x union all select 3,4) to '/allowed/out.csv' (header)",
  };
  try {
    for (const char *query : queries) {
      ObArenaAllocator allocator;
      ObParser parser(allocator, 0);
      ParseResult result;
      const int ret = parser.parse(ObString(query), result);
      if (ret != OB_SUCCESS || result.result_tree_ == nullptr) {
        std::cerr << "parser failure " << ret << " at " << result.start_col_
                  << " " << (result.error_msg_ ? result.error_msg_ : "") << ": "
                  << query << '\n';
        return 1;
      }
      const ParseNode *node = result.result_tree_->children_[0];
      if (count == 7 || count == 8) {
        if (node->type_ != T_SHOW_COLUMNS || node->children_[0] == nullptr ||
            node->children_[0]->value_ != 1701)
          throw std::runtime_error("DESCRIBE SELECT metadata marker missing");
      }
      if (count == 9 && node->type_ != T_EXPLAIN)
        throw std::runtime_error("EXPLAIN behavior changed");
      parser.free_result(result);
      ++count;
    }
    {
      ObArenaAllocator allocator;
      ObParser parser(allocator, 0);
      ParseResult result;
      const char *invalid =
          "copy (select 1 into outfile '/allowed/inner.csv') "
          "to '/allowed/out.csv'";
      if (parser.parse(ObString(invalid), result) == OB_SUCCESS)
        throw std::runtime_error("COPY source INTO was accepted");
      parser.free_result(result);
      ++count;
    }
    std::cout << "PASS " << count << " parser cases\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

#ifndef FILE_SQL_STANDALONE_TEST
#include <gtest/gtest.h>
TEST(FileSqlParser, GrammarAndOrdinarySql) {
  ASSERT_EQ(0, run_file_sql_parser_tests());
}
#endif
