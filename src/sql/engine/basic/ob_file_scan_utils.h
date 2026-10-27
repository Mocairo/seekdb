/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oceanbase {
namespace sql {
namespace file_sql {

// This module parses files only. All relational execution belongs to seekdb.
constexpr size_t MAX_COLUMNS = 1024;
constexpr size_t MAX_RECORD_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_METADATA_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_FILES = 4096;
constexpr size_t MAX_SCAN_MEMORY = 64 * 1024 * 1024;
enum class Type { NIL, BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME };
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Cell {
  std::string text;
  bool null = true;
  bool json_string = false;
  Cell() = default;
  explicit Cell(std::string value) : text(std::move(value)), null(false) {}
};
struct Column {
  std::string name;
  Type type = Type::NIL;
  bool nullable = false;
  int scale = 0;
};
struct Options {
  std::string format = "csv";
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string nullstr = "NULL";
  int64_t sample_size = -1;
};
struct Identity {
  uint64_t device = 0, inode = 0, size = 0;
  int64_t modified = 0;
  bool operator==(const Identity &o) const {
    return device == o.device && inode == o.inode && size == o.size &&
           modified == o.modified;
  }
};
struct Spec {
  std::string path, secure_root;
  Options options;
  Identity identity;
  std::vector<Column> columns;
  uint64_t rows = 0;
  std::string encode() const;
  static Spec decode(const std::string &encoded);
};

bool valid_utf8(const std::string &text);
Type infer_type(const Cell &cell);
Type widen(Type a, Type b);
const char *type_name(Type type);
std::string secure_path(const std::string &path, const std::string &root);
Identity file_identity(const std::string &path);
std::vector<std::string> discover(const std::string &pattern,
                                  const std::string &root);
Spec bind(const std::string &path, const std::string &root,
          const Options &options);

class Reader {
public:
  explicit Reader(Spec spec);
  ~Reader();
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  void open(const std::vector<size_t> &projection = {});
  bool next(std::vector<Cell> &row);
  size_t next_batch(size_t capacity, std::vector<std::vector<Cell>> &batch);
  void rescan();
  void check_unchanged() const;
  const Spec &spec() const { return spec_; }

private:
  struct Parquet;
  Spec spec_;
  FILE *file_ = nullptr;
  std::unique_ptr<Parquet> parquet_;
  std::vector<size_t> projection_;
  uint64_t line_ = 1, record_ = 0;
  bool csv_record(std::vector<Cell> &row);
  bool json_record(std::vector<std::pair<std::string, Cell>> &object);
  friend Spec bind(const std::string &, const std::string &, const Options &);
};

// Commit a CSV export only after successful completion. Existing targets are
// never overwritten; destruction removes the private incomplete temporary file.
class CsvWriter {
public:
  CsvWriter(const std::string &path, const std::string &root, Options options);
  ~CsvWriter();
  void write(const std::vector<Cell> &row);
  void commit();

private:
  std::string target_, temporary_, root_;
  Options options_;
  FILE *file_ = nullptr;
  bool committed_ = false;
  bool temporary_created_ = false;
#ifndef _WIN32
  int directory_fd_ = -1;
#else
  std::vector<intptr_t> directory_handles_;
  std::wstring temporary_native_;
#endif
  std::string target_name_, temporary_name_;
  void cleanup() noexcept;
};

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
