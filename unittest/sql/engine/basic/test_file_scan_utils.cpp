/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <iostream>

#include "sql/engine/basic/ob_file_scan_utils.h"

using namespace oceanbase::sql::file_sql;
namespace fs = std::filesystem;
static int checks = 0;
static void check(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
static void fails(const std::function<void()> &fn, const char *message) {
  bool failed = false;
  try {
    fn();
  } catch (const Error &) {
    failed = true;
  }
  check(failed, message);
}
static void put(const fs::path &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
  if (!out)
    throw std::runtime_error("fixture write failed");
}
static std::string read_bytes(const fs::path &path) {
  std::ifstream source(path, std::ios::binary);
  if (!source)
    throw std::runtime_error("cannot read test fixture");
  return std::string(std::istreambuf_iterator<char>(source), {});
}
static void parquet_fixture(const fs::path &target) {
  const fs::path source = fs::path(__FILE__).parent_path() / "data" /
                          (target.filename().string() + ".b64");
  const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  uint32_t buffer = 0;
  int bits = -8;
  for (unsigned char character : read_bytes(source)) {
    if (character == '=')
      break;
    const auto value = alphabet.find(char(character));
    if (value == std::string::npos)
      continue;
    buffer = (buffer << 6) | uint32_t(value);
    bits += 6;
    if (bits >= 0) {
      result += char((buffer >> bits) & 255);
      bits -= 8;
    }
  }
  put(target, result);
}
static size_t open_handles() {
#ifdef _WIN32
  DWORD handles = 0;
  if (!GetProcessHandleCount(GetCurrentProcess(), &handles))
    throw std::runtime_error("cannot count test handles");
  return handles;
#elif defined(__linux__)
  size_t count = 0;
  for (const auto &unused : fs::directory_iterator("/proc/self/fd")) {
    (void)unused;
    ++count;
  }
  return count;
#else
  return 0;
#endif
}
static std::string empty_date_parquet(bool valid_physical, bool row_count) {
  // A compact-Thrift flat DATE schema with zero row groups. DATE must use
  // INT32; using INT64 is malformed even when there are no data pages.
  std::string metadata{char(0x15), char(2),    char(0x19),
                       char(0x2c), char(0x48), char(6)};
  metadata += "schema";
  metadata += std::string{char(0x15),
                          char(2),
                          char(0),
                          char(0x15),
                          char(valid_physical ? 2 : 4),
                          char(0x25),
                          char(2),
                          char(0x18),
                          char(1),
                          'x',
                          char(0x25),
                          char(12),
                          char(0)};
  if (row_count)
    metadata += std::string{char(0x16), char(0), char(0x19)};
  else
    metadata += char(0x29);
  metadata += std::string{char(0x0c), char(0)};
  std::string result = "PAR1" + metadata;
  for (unsigned i = 0; i < 4; ++i)
    result += char((metadata.size() >> (i * 8)) & 255);
  return result + "PAR1";
}
int run_file_scan_tests(int argc, char **argv) {
  fs::path root = fs::absolute(argc > 1 ? argv[1] : "file_scan_test_data");
  fs::create_directories(root);
  const std::string secure = root.generic_string();
  auto path = [&](const char *name) { return (root / name).generic_string(); };
  Options csv;
  try {
    put(path("basic.csv"),
        "id,name,value,active,day,at,empty,id\r\n"
        "1,\"hello,\r\nworld\",1.25,true,2024-02-29,2024-02-29 "
        "23:59:59,NULL,9\r\n"
        "2,\"say \"\"hi\"\"\",2,false,,,NULL,10\r\n");
    Spec s = oceanbase::sql::file_sql::bind(path("basic.csv"), secure, csv);
    check(s.rows == 2 && s.columns.size() == 8, "CSV schema/row count");
    check(s.columns[7].name == "id__2", "duplicate header");
    check(s.columns[2].type == Type::DOUBLE &&
              s.columns[3].type == Type::BOOLEAN,
          "numeric widening/boolean");
    check(s.columns[4].type == Type::DATE &&
              s.columns[5].type == Type::DATETIME,
          "date/datetime inference");
    check(s.columns[5].scale == 0, "whole-second datetime precision");
    put(path("precision.csv"), "at\n2024-02-29\n2024-02-29 "
                               "01:02:03.123\n2024-02-29 01:02:03.123456\n");
    const Spec precision =
        oceanbase::sql::file_sql::bind(path("precision.csv"), secure, csv);
    check(precision.columns[0].type == Type::DATETIME &&
              precision.columns[0].scale == 6 &&
              Spec::decode(precision.encode()).columns[0].scale == 6,
          "datetime precision widening/serialization");
    put(path("precision.jsonl"),
        "{\"at\":null}\n{\"at\":\"2024-02-29 01:02:03.123\"}\n");
    Options precision_json;
    precision_json.format = "json";
    const Spec json_precision = oceanbase::sql::file_sql::bind(
        path("precision.jsonl"), secure, precision_json);
    check(json_precision.columns[0].type == Type::DATETIME &&
              json_precision.columns[0].nullable &&
              json_precision.columns[0].scale == 3,
          "JSON datetime precision after NULL");
    check(s.columns[6].type == Type::NIL && s.columns[6].nullable,
          "all NULL column");
    Reader reader(Spec::decode(s.encode()));
    reader.open({7, 1, 4, 6});
    std::vector<Cell> row;
    check(reader.next(row) && row[0].text == "9" &&
              row[1].text == "hello,\r\nworld",
          "projection and embedded newline");
    check(reader.next(row) && row[1].text == "say \"hi\"" && row[2].null &&
              row[3].null,
          "quoted escape and NULL");
    check(!reader.next(row), "EOF");
    reader.rescan();
    std::vector<std::vector<Cell>> batch;
    check(reader.next_batch(17, batch) == 2 && batch[1][0].text == "10",
          "batch/rescan");

    put(path("noheader.csv"), "1;\"\";NULL\n2;\"NULL\";3\n");
    auto custom = csv;
    custom.header = false;
    custom.delimiter = ';';
    auto ns =
        oceanbase::sql::file_sql::bind(path("noheader.csv"), secure, custom);
    Reader nr(ns);
    nr.open();
    nr.next(row);
    check(ns.columns[0].name == "column1" && !row[1].null &&
              row[1].text.empty() && row[2].null,
          "header=false and quoted empty text");
    put(path("escape.csv"), "a\n\"x\\\"y\\\\z\"\n");
    custom = csv;
    custom.escape = '\\';
    Reader er(
        oceanbase::sql::file_sql::bind(path("escape.csv"), secure, custom));
    er.open();
    er.next(row);
    check(row[0].text == "x\"y\\z", "custom quote escape");

    Options json;
    json.format = "json";
    put(path("union.jsonl"),
        "{\"id\":1,\"x\":2,\"s\":\"\\uD83D\\uDE00\"}\r\n"
        "{\"id\":2,\"x\":2.5,\"extra\":true}\n{\"id\":3,\"x\":\"other\",\"s\":"
        "null}\n");
    auto js = oceanbase::sql::file_sql::bind(path("union.jsonl"), secure, json);
    check(js.columns.size() == 4 && js.columns[1].type == Type::VARCHAR &&
              js.columns[2].nullable && js.columns[3].nullable,
          "JSON union/type conflict");
    Reader jr(js);
    jr.open({3, 2});
    jr.next(row);
    check(row[0].null && row[1].text == "\xf0\x9f\x98\x80",
          "JSON missing/unicode");
    jr.next(row);
    check(row[0].text == "true" && row[1].null, "JSON late field");
    for (const auto &bad :
         {"{\"a\":[1]}\n", "{\"a\":{}}\n", "{\"a\":1,\"a\":2}\n",
          "{\"a\":01}\n", "{\"a\":\"\\uD800\"}\n"}) {
      put(path("bad.jsonl"), bad);
      fails(
          [&] {
            oceanbase::sql::file_sql::bind(path("bad.jsonl"), secure, json);
          },
          "invalid JSON accepted");
    }
    for (const auto &bad :
         {"a,b\n1\n", "a\n\"unterminated\n", "a\n\"x\"junk\n", "a\n\xff\n"}) {
      put(path("bad.csv"), bad);
      fails(
          [&] { oceanbase::sql::file_sql::bind(path("bad.csv"), secure, csv); },
          "invalid CSV accepted");
    }
    for (const auto &text :
         {"9223372036854775807", "-9223372036854775808", "+1"})
      check(infer_type(Cell(text)) == Type::BIGINT, "int64 boundary");
    for (const auto &text :
         {"9223372036854775808", "-9223372036854775809", "1e999", "2023-02-29",
          "2024-13-01", "2024-01-01 24:00:00"})
      check(infer_type(Cell(text)) == Type::VARCHAR, "out of range inference");
    check(infer_type(Cell("1.5e2")) == Type::DOUBLE, "exponent inference");
    check(widen(Type::DATE, Type::DATETIME) == Type::DATETIME, "date widening");
    check(!valid_utf8("\xc0\x80") && !valid_utf8("\xed\xa0\x80"),
          "UTF8 rejection");
    fails([&] { oceanbase::sql::file_sql::bind(path("basic.csv"), "", csv); },
          "empty secure root");
    fs::path outside = root.parent_path() / "file_scan_outside.csv";
    put(outside, "x\n1\n");
    fails(
        [&] {
          oceanbase::sql::file_sql::bind(outside.generic_string(), secure, csv);
        },
        "path escape");
    std::error_code ec;
    fs::create_symlink(outside, root / "escape-link.csv", ec);
    if (!ec)
      fails(
          [&] {
            oceanbase::sql::file_sql::bind(path("escape-link.csv"), secure,
                                           csv);
          },
          "symlink escape");
    fs::remove(root / "escape-link.csv", ec);
    fs::remove(outside);
    auto invalid = csv;
    invalid.sample_size = 1;
    fails(
        [&] {
          oceanbase::sql::file_sql::bind(path("basic.csv"), secure, invalid);
        },
        "partial inference accepted");
    check(discover(path("basic.*"), secure).size() == 1, "glob");

    put(path("changing.csv"), "id\n1\n");
    auto change =
        oceanbase::sql::file_sql::bind(path("changing.csv"), secure, csv);
    Reader cr(change);
    cr.open();
    put(path("changing.csv"), "id\n12345\n");
    fails([&] { cr.next(row); }, "input mutation missed");
    fails([&] { cr.rescan(); }, "rescan mutation missed");

    fs::remove(path("export.csv"), ec);
    {
      CsvWriter w(path("export.csv"), secure, csv);
      w.write({Cell("id"), Cell("text"), Cell("null")});
      w.write({Cell("1"), Cell("quote\"\nand,comma"), Cell()});
      w.write({Cell("2"), Cell(""), Cell("NULL")});
      w.commit();
    }
    Reader xr(oceanbase::sql::file_sql::bind(path("export.csv"), secure, csv));
    xr.open();
    xr.next(row);
    check(row[1].text == "quote\"\nand,comma" && row[2].null,
          "CSV export round trip");
    xr.next(row);
    check(!row[1].null && row[1].text.empty() && row[2].text == "NULL" &&
              !row[2].null,
          "CSV export NULL/string distinction");
    fails([&] { CsvWriter w(path("export.csv"), secure, csv); },
          "overwrite accepted");
    fs::remove(path("aborted.csv"), ec);
    {
      CsvWriter w(path("aborted.csv"), secure, csv);
      w.write({Cell("discard")});
    }
    check(!fs::exists(path("aborted.csv")) &&
              discover(path("*.tmp"), secure).empty(),
          "aborted export cleanup");

    // Independently generated PyArrow fixtures exercise actual Parquet
    // encoders, Snappy, nullable pages, row groups, CRCs and projection without
    // a runtime dependency on PyArrow. The generator is retained for
    // reproducibility.
    Options parquet;
    parquet.format = "parquet";
    for (const char *name :
         {"plain.parquet", "dictionary.parquet", "v2.parquet"}) {
      parquet_fixture(path(name));
      auto ps = oceanbase::sql::file_sql::bind(path(name), secure, parquet);
      check(ps.rows == 257 && ps.columns.size() == 5, "Parquet footer schema");
      Reader pr(ps);
      pr.open({4, 0, 2, 1, 3});
      int count = 0;
      while (pr.next(row)) {
        check(row[1].text == std::to_string(count),
              "Parquet row/group ordering");
        check(row[0].null == (count % 7 == 0), "Parquet nullable string");
        if (!row[0].null)
          check(row[0].text == "value-" + std::to_string(count % 3),
                "Parquet dictionary/string");
        check(row[2].text == (count % 2 ? "true" : "false"), "Parquet boolean");
        ++count;
      }
      check(count == 257, "Parquet row count");
      pr.rescan();
      check(pr.next(row) && row[1].text == "0", "Parquet rescan");
    }
    std::string corrupt = read_bytes(path("plain.parquet"));
    corrupt.at(100) ^= 1;
    put(path("corrupt.parquet"), corrupt);
    auto corrupt_spec = oceanbase::sql::file_sql::bind(path("corrupt.parquet"),
                                                       secure, parquet);
    fails(
        [&] {
          Reader invalid(corrupt_spec);
          invalid.open({0});
          invalid.next(row);
        },
        "Parquet CRC ignored");
    Reader cropped(corrupt_spec);
    cropped.open({1});
    check(cropped.next(row) && row[0].text == "0",
          "Parquet read an unprojected corrupt column");
    parquet_fixture(path("empty.parquet"));
    Reader empty_arrow(
        oceanbase::sql::file_sql::bind(path("empty.parquet"), secure, parquet));
    empty_arrow.open();
    check(!empty_arrow.next(row), "empty Arrow row group");
    put(path("empty-date.parquet"), empty_date_parquet(true, true));
    Reader empty_date(oceanbase::sql::file_sql::bind(path("empty-date.parquet"),
                                                     secure, parquet));
    empty_date.open();
    check(!empty_date.next(row), "empty Parquet row count");
    put(path("bad-schema.parquet"), empty_date_parquet(false, true));
    fails(
        [&] {
          oceanbase::sql::file_sql::bind(path("bad-schema.parquet"), secure,
                                         parquet);
        },
        "Parquet logical/physical mismatch accepted");
    put(path("missing-metadata.parquet"), empty_date_parquet(true, false));
    fails(
        [&] {
          oceanbase::sql::file_sql::bind(path("missing-metadata.parquet"),
                                         secure, parquet);
        },
        "incomplete Parquet metadata accepted");
    const size_t before = open_handles();
    for (int iteration = 0; iteration < 100; ++iteration) {
      Reader probe(s);
      probe.open({0});
      probe.next(row);
      probe.rescan();
      probe.next(row);
    }
    check(open_handles() == before, "file descriptor or handle leak");
#ifdef _WIN32
    fails([&] { CsvWriter ads(path("basic.csv") + ":stream", secure, csv); },
          "NTFS ADS output accepted");
    fails([&] { secure_path("//untrusted.invalid/share/file", secure); },
          "UNC input accepted");
#endif
    fs::remove(path("after-commit.csv"), ec);
    {
      CsvWriter writer(path("after-commit.csv"), secure, csv);
      writer.write({Cell("x")});
      writer.commit();
      fails([&] { writer.write({Cell("y")}); }, "write after commit accepted");
    }
    fs::remove(path("large-null.csv"), ec);
    auto large_null = csv;
    large_null.nullstr.assign(1024 * 1024, 'N');
    fails(
        [&] {
          CsvWriter writer(path("large-null.csv"), secure, large_null);
          writer.write(std::vector<Cell>(17));
        },
        "export NULL text resource limit ignored");
    check(!fs::exists(path("large-null.csv")) &&
              discover(path("*.tmp"), secure).empty(),
          "large export cleanup");
#if defined(__linux__)
    // A directory descriptor survives rename. Publishing must revalidate its
    // actual location rather than trusting a replacement at the old path.
    const auto export_dir = root / "renamed-output";
    const auto moved_dir =
        root.parent_path() / (root.filename().string() + "-moved-output");
    fs::create_directory(export_dir);
    bool refused = false;
    {
      CsvWriter writer((export_dir / "export.csv").generic_string(), secure,
                       csv);
      writer.write({Cell("must stay within the allowed directory")});
      fs::rename(export_dir, moved_dir);
      fs::create_directory(export_dir);
      try {
        writer.commit();
      } catch (const Error &) {
        refused = true;
      }
    }
    const bool absent = !fs::exists(moved_dir / "export.csv");
    fs::remove(moved_dir / "export.csv");
    fs::remove(moved_dir);
    fs::remove(export_dir);
    check(refused, "renamed export directory accepted");
    check(absent, "export escaped through a renamed directory");
#endif
    std::cout << "PASS " << checks << " checks\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
    return 1;
  }
}

#ifndef FILE_SQL_STANDALONE_TEST
#include <gtest/gtest.h>

#include <cstdlib>
TEST(FileScanUtils, Acceptance) {
  const char *temporary = std::getenv("TEST_TMPDIR");
  std::string path =
      std::string(temporary ? temporary : ".") + "/file_scan_data";
  char executable[] = "file_scan_test";
  char *arguments[] = {executable, path.data()};
  ASSERT_EQ(0, run_file_scan_tests(2, arguments));
}
#endif
