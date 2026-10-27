/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#include "sql/engine/basic/ob_file_scan_utils.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase {
namespace sql {
namespace file_sql {
namespace fs = std::filesystem;
static std::string path_text(const fs::path &path) {
  auto text = path.u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}
namespace {
std::string lower(std::string s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z')
      c += 32;
  return s;
}
bool below(fs::path p, fs::path root) {
  auto a = p.begin(), b = root.begin();
  for (; b != root.end(); ++a, ++b) {
    if (a == p.end())
      return false;
#ifdef _WIN32
    if (CompareStringOrdinal(a->c_str(), -1, b->c_str(), -1, TRUE) !=
        CSTR_EQUAL)
      return false;
#else
    if (*a != *b)
      return false;
#endif
  }
  return true;
}
void require(bool ok, const std::string &message) {
  if (!ok)
    throw Error(message);
}
void local_path_only(const std::string &text) {
  require(text.size() <= 32768, "file path exceeds resource limit");
  require(text.find('\0') == std::string::npos, "NUL in file path");
#ifdef _WIN32
  const fs::path path = fs::u8path(text);
  const std::wstring drive = path.root_name().native();
  require(drive.empty() || (drive.size() == 2 && drive[1] == L':'),
          "UNC and device paths are not supported by local file SQL");
  for (const auto &component : path.relative_path())
    require(component.native().find(L':') == std::wstring::npos,
            "NTFS alternate data streams are not supported");
#endif
}
void validate_options(const Options &o) {
  require(o.nullstr.size() <= MAX_RECORD_BYTES && valid_utf8(o.nullstr),
          "invalid or oversized CSV NULL text");
  require(o.sample_size == -1,
          "sample_size: only -1 (full schema inference) is supported");
  require(o.format == "csv" || o.format == "json" || o.format == "ndjson" ||
              o.format == "parquet" || o.format == "glob",
          "unsupported file format");
  require(o.delimiter && o.quote && o.escape && o.delimiter != o.quote &&
              o.delimiter != '\n' && o.delimiter != '\r' && o.quote != '\n' &&
              o.quote != '\r' && o.escape != '\n' && o.escape != '\r',
          "invalid CSV delimiter, quote or escape");
  require(o.nullstr.find_first_of("\r\n") == std::string::npos &&
              o.nullstr.find(o.delimiter) == std::string::npos &&
              o.nullstr.find(o.quote) == std::string::npos,
          "NULL text must be an unquoted CSV field");
}
bool date_valid(const std::string &s) {
  if (s.size() < 10 || s[4] != '-' || s[7] != '-')
    return false;
  for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
    if (s[i] < '0' || s[i] > '9')
      return false;
  int y = std::stoi(s.substr(0, 4)), m = std::stoi(s.substr(5, 2)),
      d = std::stoi(s.substr(8, 2));
  static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return y >= 1 && m >= 1 && m <= 12 && d >= 1 &&
         d <=
             days[m] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}
bool datetime_valid(const std::string &s) {
  if (s.size() < 19 || !date_valid(s) || (s[10] != ' ' && s[10] != 'T') ||
      s[13] != ':' || s[16] != ':')
    return false;
  for (size_t i : {11, 12, 14, 15, 17, 18})
    if (s[i] < '0' || s[i] > '9')
      return false;
  if (std::stoi(s.substr(11, 2)) > 23 || std::stoi(s.substr(14, 2)) > 59 ||
      std::stoi(s.substr(17, 2)) > 59)
    return false;
  if (s.size() == 19)
    return true;
  if (s[19] != '.' || s.size() < 21 || s.size() > 26)
    return false;
  return std::all_of(s.begin() + 20, s.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}
bool integer_text(const std::string &s) {
  size_t i = !s.empty() && (s[0] == '-' || s[0] == '+') ? 1 : 0;
  return i < s.size() && std::all_of(
                             s.begin() + i, s.end(),
                             [](char c) { return c >= '0' && c <= '9'; });
}
void append_utf8(std::string &s, uint32_t c) {
  require(c <= 0x10ffff && !(c >= 0xd800 && c <= 0xdfff),
          "invalid JSON Unicode escape");
  if (c < 128)
    s += char(c);
  else if (c < 2048) {
    s += char(0xc0 | (c >> 6));
    s += char(0x80 | (c & 63));
  } else if (c < 65536) {
    s += char(0xe0 | (c >> 12));
    s += char(0x80 | ((c >> 6) & 63));
    s += char(0x80 | (c & 63));
  } else {
    s += char(0xf0 | (c >> 18));
    s += char(0x80 | ((c >> 12) & 63));
    s += char(0x80 | ((c >> 6) & 63));
    s += char(0x80 | (c & 63));
  }
}
struct JsonLine {
  const std::string &s;
  size_t i = 0;
  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r'))
      ++i;
  }
  void take(char c) {
    ws();
    require(i < s.size() && s[i] == c,
            "invalid JSON syntax at byte " + std::to_string(i));
    ++i;
  }
  uint32_t hex4() {
    uint32_t n = 0;
    for (int j = 0; j < 4; ++j) {
      require(i < s.size(), "truncated Unicode escape");
      char c = s[i++];
      int v = c >= '0' && c <= '9'   ? c - '0'
              : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                     : -1;
      require(v >= 0, "invalid Unicode escape");
      n = (n << 4) | v;
    }
    return n;
  }
  std::string string() {
    take('"');
    std::string out;
    while (i < s.size()) {
      unsigned char c = s[i++];
      if (c == '"') {
        require(valid_utf8(out), "invalid UTF-8 JSON string");
        return out;
      }
      require(c >= 32, "unescaped control character in JSON string");
      if (c != '\\') {
        out += char(c);
        continue;
      }
      require(i < s.size(), "truncated JSON escape");
      c = s[i++];
      switch (c) {
      case '"':
      case '\\':
      case '/':
        out += char(c);
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        uint32_t u = hex4();
        if (u >= 0xd800 && u <= 0xdbff) {
          require(i + 2 <= s.size() && s[i] == '\\' && s[i + 1] == 'u',
                  "missing low surrogate");
          i += 2;
          uint32_t low = hex4();
          require(low >= 0xdc00 && low <= 0xdfff, "invalid low surrogate");
          u = 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00);
        }
        append_utf8(out, u);
        break;
      }
      default:
        throw Error("invalid JSON escape");
      }
    }
    throw Error("unterminated JSON string");
  }
  Cell value() {
    ws();
    require(i < s.size(), "missing JSON value");
    if (s[i] == '"') {
      Cell cell(string());
      cell.json_string = true;
      return cell;
    }
    require(s[i] != '{' && s[i] != '[',
            "nested JSON objects and arrays are not supported");
    size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ' ' &&
           s[i] != '\t' && s[i] != '\r')
      ++i;
    std::string v = s.substr(start, i - start);
    if (v == "null")
      return Cell();
    if (v == "true" || v == "false")
      return Cell(v);
    size_t p = 0;
    if (p < v.size() && v[p] == '-')
      ++p;
    require(p < v.size(), "invalid JSON scalar");
    if (v[p] == '0')
      ++p;
    else {
      require(v[p] >= '1' && v[p] <= '9', "invalid JSON number");
      while (p < v.size() && isdigit(static_cast<unsigned char>(v[p])))
        ++p;
    }
    if (p < v.size() && v[p] == '.') {
      ++p;
      size_t q = p;
      while (p < v.size() && isdigit(static_cast<unsigned char>(v[p])))
        ++p;
      require(p > q, "invalid JSON fraction");
    }
    if (p < v.size() && (v[p] == 'e' || v[p] == 'E')) {
      ++p;
      if (p < v.size() && (v[p] == '-' || v[p] == '+'))
        ++p;
      size_t q = p;
      while (p < v.size() && isdigit(static_cast<unsigned char>(v[p])))
        ++p;
      require(p > q, "invalid JSON exponent");
    }
    require(p == v.size(), "invalid JSON number");
    return Cell(v);
  }
  std::vector<std::pair<std::string, Cell>> object() {
    std::vector<std::pair<std::string, Cell>> out;
    std::set<std::string> names;
    take('{');
    ws();
    if (i < s.size() && s[i] == '}')
      ++i;
    else
      for (;;) {
        require(out.size() < MAX_COLUMNS, "too many JSON fields");
        std::string key = string();
        require(!key.empty() && names.insert(lower(key)).second,
                "empty or duplicate JSON field");
        take(':');
        out.emplace_back(key, value());
        ws();
        require(i < s.size(), "unterminated JSON object");
        if (s[i++] == '}')
          break;
        require(s[i - 1] == ',', "invalid JSON field separator");
      }
    ws();
    require(i == s.size(), "trailing content after JSON object");
    return out;
  }
};
FILE *open_input(const std::string &path) {
#ifdef _WIN32
  return _wfopen(fs::u8path(path).c_str(), L"rb");
#else
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return nullptr;
  FILE *f = fdopen(fd, "rb");
  if (!f)
    close(fd);
  return f;
#endif
}
Identity opened_identity(FILE *f) {
  Identity id;
#ifdef _WIN32
  BY_HANDLE_FILE_INFORMATION info{};
  require(GetFileInformationByHandle(
              reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f))), &info) != 0,
          "cannot stat open file");
  require(!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY),
          "input must be a regular file");
  id.device = info.dwVolumeSerialNumber;
  id.inode = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  id.size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  id.modified = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) |
                info.ftLastWriteTime.dwLowDateTime;
#else
  struct stat st{};
  require(fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode),
          "input must be a regular file");
  id.device = st.st_dev;
  id.inode = st.st_ino;
  id.size = st.st_size;
#ifdef __APPLE__
  id.modified = st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
  id.modified = st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
#endif
  return id;
}
std::string opened_path(FILE *f, const std::string &fallback) {
  (void)fallback;
#ifdef _WIN32
  std::vector<wchar_t> name(32768);
  DWORD n = GetFinalPathNameByHandleW(
      reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f))), name.data(),
      static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED);
  require(n > 0 && n < name.size(), "cannot resolve open file path");
  std::wstring w(name.data(), n);
  if (w.rfind(L"\\\\?\\UNC\\", 0) == 0)
    w = L"\\\\" + w.substr(8);
  else if (w.rfind(L"\\\\?\\", 0) == 0)
    w = w.substr(4);
  return path_text(fs::path(w));
#elif defined(__linux__)
  std::vector<char> name(32768);
  ssize_t n = readlink(("/proc/self/fd/" + std::to_string(fileno(f))).c_str(),
                       name.data(), name.size());
  require(n > 0 && size_t(n) < name.size(), "cannot resolve open file path");
  return std::string(name.data(), n);
#else
  return fallback;
#endif
}
} // namespace

bool valid_utf8(const std::string &s) {
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i++];
    if (c < 128)
      continue;
    unsigned n = c >= 0xc2 && c <= 0xdf   ? 1
                 : c >= 0xe0 && c <= 0xef ? 2
                 : c >= 0xf0 && c <= 0xf4 ? 3
                                          : 0;
    if (!n || i + n > s.size())
      return false;
    uint32_t u = c & ((1U << (6 - n)) - 1);
    for (unsigned j = 0; j < n; ++j) {
      unsigned char d = s[i++];
      if ((d & 0xc0) != 0x80)
        return false;
      u = (u << 6) | (d & 63);
    }
    if ((n == 1 && u < 128) || (n == 2 && u < 2048) || (n == 3 && u < 65536) ||
        u > 0x10ffff || (u >= 0xd800 && u <= 0xdfff))
      return false;
  }
  return true;
}
Type infer_type(const Cell &cell) {
  if (cell.null)
    return Type::NIL;
  const auto &s = cell.text;
  if (cell.json_string) {
    if (s.size() == 10 && date_valid(s))
      return Type::DATE;
    if (datetime_valid(s))
      return Type::DATETIME;
    return Type::VARCHAR;
  }
  if (s == "true" || s == "false" || s == "TRUE" || s == "FALSE")
    return Type::BOOLEAN;
  if (integer_text(s)) {
    int64_t n = 0;
    const char *b = s.data() + (s[0] == '+');
    auto r = std::from_chars(b, s.data() + s.size(), n);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() ? Type::BIGINT
                                                               : Type::VARCHAR;
  }
  if (!s.empty() && s.find_first_of(".eE") != std::string::npos) {
    std::istringstream in(s);
    in.imbue(std::locale::classic());
    double d;
    in >> std::noskipws >> d;
    if (in && in.eof() && std::isfinite(d))
      return Type::DOUBLE;
  }
  if (s.size() == 10 && date_valid(s))
    return Type::DATE;
  if (datetime_valid(s))
    return Type::DATETIME;
  return Type::VARCHAR;
}
Type widen(Type a, Type b) {
  if (a == Type::NIL)
    return b;
  if (b == Type::NIL || a == b)
    return a;
  if ((a == Type::BIGINT && b == Type::DOUBLE) ||
      (b == Type::BIGINT && a == Type::DOUBLE))
    return Type::DOUBLE;
  if ((a == Type::DATE && b == Type::DATETIME) ||
      (b == Type::DATE && a == Type::DATETIME))
    return Type::DATETIME;
  return Type::VARCHAR;
}
const char *type_name(Type t) {
  static const char *n[] = {"NULL",    "BIGINT", "DOUBLE",  "BOOLEAN",
                            "VARCHAR", "DATE",   "DATETIME"};
  return n[static_cast<unsigned>(t)];
}
std::string secure_path(const std::string &path, const std::string &root) {
  local_path_only(path);
  local_path_only(root);
  require(!root.empty() && lower(root) != "null",
          "secure_file_priv does not permit local file access");
  require(path.find('\0') == std::string::npos &&
              root.find('\0') == std::string::npos,
          "NUL in file path");
  std::error_code ec;
  auto r = fs::canonical(fs::u8path(root), ec);
  require(!ec && fs::is_directory(r), "invalid secure_file_priv directory");
  auto p = fs::canonical(fs::u8path(path), ec);
  require(!ec, "file does not exist: " + path);
  require(below(p, r), "file path escapes secure_file_priv: " + path);
  return path_text(p);
}
Identity file_identity(const std::string &path) {
  FILE *f = open_input(path);
  require(f != nullptr, "cannot open file: " + path);
  try {
    auto id = opened_identity(f);
    fclose(f);
    return id;
  } catch (...) {
    fclose(f);
    throw;
  }
}
std::vector<std::string> discover(const std::string &pattern,
                                  const std::string &root) {
  local_path_only(pattern);
  fs::path p = fs::u8path(pattern);
  auto parent = p.parent_path();
  if (parent.empty())
    parent = ".";
  std::string dir = secure_path(path_text(parent), root),
              pat = path_text(p.filename());
  require(pat.size() < 4096, "glob pattern too long");
  auto matches = [](const std::string &s, const std::string &p) {
    size_t i = 0, j = 0, star = std::string::npos, last = 0;
    while (i < s.size()) {
      if (j < p.size() && (p[j] == '?' || p[j] == s[i])) {
        ++i;
        ++j;
      } else if (j < p.size() && p[j] == '*') {
        star = j++;
        last = i;
      } else if (star != std::string::npos) {
        j = star + 1;
        i = ++last;
      } else
        return false;
    }
    while (j < p.size() && p[j] == '*')
      ++j;
    return j == p.size();
  };
  std::vector<std::string> out;
  for (const auto &e : fs::directory_iterator(fs::u8path(dir))) {
    if (matches(path_text(e.path().filename()), pat)) {
      std::string actual = secure_path(path_text(e.path()), root);
      if (fs::is_regular_file(fs::u8path(actual))) {
        require(out.size() < MAX_FILES, "glob result limit exceeded");
        out.push_back(actual);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string Spec::encode() const {
  std::ostringstream o;
  o.imbue(std::locale::classic());
  o << "FSQ1 " << std::quoted(path) << ' ' << std::quoted(secure_root) << ' '
    << std::quoted(options.format) << ' ' << options.header << ' '
    << int(static_cast<unsigned char>(options.delimiter)) << ' '
    << int(static_cast<unsigned char>(options.quote)) << ' '
    << int(static_cast<unsigned char>(options.escape)) << ' '
    << std::quoted(options.nullstr) << ' ' << identity.device << ' '
    << identity.inode << ' ' << identity.size << ' ' << identity.modified << ' '
    << rows << ' ' << columns.size();
  for (auto &c : columns)
    o << ' ' << std::quoted(c.name) << ' ' << int(c.type) << ' ' << c.nullable
      << ' ' << c.scale;
  return o.str();
}
Spec Spec::decode(const std::string &s) {
  require(s.size() <= MAX_METADATA_BYTES, "file descriptor too large");
  std::istringstream in(s);
  in.imbue(std::locale::classic());
  std::string magic;
  Spec r;
  int d, q, e;
  size_t n;
  in >> magic >> std::quoted(r.path) >> std::quoted(r.secure_root) >>
      std::quoted(r.options.format) >> r.options.header >> d >> q >> e >>
      std::quoted(r.options.nullstr) >> r.identity.device >> r.identity.inode >>
      r.identity.size >> r.identity.modified >> r.rows >> n;
  require(in && magic == "FSQ1" && n <= MAX_COLUMNS && d > 0 && d < 256 &&
              q > 0 && q < 256 && e > 0 && e < 256,
          "invalid file scan descriptor");
  r.options.delimiter = char(d);
  r.options.quote = char(q);
  r.options.escape = char(e);
  for (size_t i = 0; i < n; ++i) {
    Column c;
    int t;
    in >> std::quoted(c.name) >> t >> c.nullable >> c.scale;
    require(in && t >= 0 && t <= int(Type::DATETIME) && c.scale >= 0 &&
                c.scale <= 6,
            "invalid file column descriptor");
    c.type = Type(t);
    r.columns.push_back(c);
  }
  in >> std::ws;
  require(in.eof(), "trailing file scan descriptor data");
  validate_options(r.options);
  return r;
}

bool Reader::csv_record(std::vector<Cell> &row) {
  try {
    row.clear();
    std::string field;
    bool quoted = false, in_quote = false, closed = false, any = false;
    size_t bytes = 0;
    auto finish = [&]() {
      require(row.size() < MAX_COLUMNS, "CSV column limit exceeded");
      require(valid_utf8(field), "invalid UTF-8 field");
      Cell c(field);
      c.null = !quoted && (field.empty() || field == spec_.options.nullstr);
      row.push_back(std::move(c));
      field.clear();
      quoted = false;
      closed = false;
    };
    for (;;) {
      int x = fgetc(file_);
      if (x == EOF) {
        require(!ferror(file_), "file read error");
        require(!in_quote,
                "unterminated CSV quote at line " + std::to_string(line_));
        if (!any)
          return false;
        finish();
        ++record_;
        return true;
      }
      any = true;
      require(++bytes <= MAX_RECORD_BYTES,
              "CSV record exceeds resource limit at line " +
                  std::to_string(line_));
      char c = char(x);
      if (in_quote) {
        if (c == spec_.options.escape &&
            spec_.options.escape != spec_.options.quote) {
          int y = fgetc(file_);
          require(y != EOF, "truncated CSV escape");
          require(++bytes <= MAX_RECORD_BYTES, "CSV record too large");
          field += char(y);
          if (y == '\n')
            ++line_;
        } else if (c == spec_.options.quote) {
          int y = fgetc(file_);
          if (y == spec_.options.quote) {
            field += c;
            ++bytes;
          } else {
            if (y != EOF)
              ungetc(y, file_);
            in_quote = false;
            closed = true;
          }
        } else {
          field += c;
          if (c == '\n')
            ++line_;
        }
      } else if (c == spec_.options.delimiter)
        finish();
      else if (c == '\n' || c == '\r') {
        if (c == '\r') {
          int y = fgetc(file_);
          if (y != '\n' && y != EOF)
            ungetc(y, file_);
        }
        ++line_;
        finish();
        ++record_;
        return true;
      } else if (c == spec_.options.quote) {
        require(field.empty() && !closed,
                "unexpected CSV quote at line " + std::to_string(line_));
        in_quote = true;
        quoted = true;
      } else {
        require(!closed, "content after CSV closing quote at line " +
                             std::to_string(line_));
        field += c;
      }
    }
  } catch (const Error &e) {
    throw Error(spec_.path + ":" + std::to_string(line_) + ": " + e.what());
  }
}
bool Reader::json_record(std::vector<std::pair<std::string, Cell>> &obj) {
  for (;;) {
    std::string s;
    int c;
    while ((c = fgetc(file_)) != EOF && c != '\n') {
      require(s.size() < MAX_RECORD_BYTES,
              "JSONL record too large at line " + std::to_string(line_));
      s += char(c);
    }
    require(!ferror(file_), "JSONL read error");
    if (c == EOF && s.empty())
      return false;
    uint64_t at = line_++;
    if (s.find_first_not_of(" \t\r") == std::string::npos)
      continue;
    try {
      require(valid_utf8(s), "invalid UTF-8");
      obj = JsonLine{s}.object();
      ++record_;
      return true;
    } catch (const Error &e) {
      throw Error(spec_.path + ":" + std::to_string(at) + ": " + e.what());
    }
  }
}

// Parquet implementation lives in the companion translation unit and uses the
// same bounded Reader interface. Text readers never retain earlier records.
#include "sql/engine/basic/ob_file_scan_parquet.inc"

Reader::Reader(Spec s) : spec_(std::move(s)) {}
Reader::~Reader() {
  parquet_.reset();
  if (file_)
    fclose(file_);
}
void Reader::check_unchanged() const {
  require(file_ && opened_identity(file_) == spec_.identity &&
              file_identity(spec_.path) == spec_.identity,
          "input file changed: " + spec_.path);
}
void Reader::open(const std::vector<size_t> &projection) {
  validate_options(spec_.options);
  parquet_.reset();
  if (file_) {
    fclose(file_);
    file_ = nullptr;
  }
  require(spec_.options.format != "glob", "glob requires discovery iterator");
  std::string actual = secure_path(spec_.path, spec_.secure_root);
  file_ = open_input(actual);
  require(file_, "cannot open input: " + actual);
  require(secure_path(opened_path(file_, actual), spec_.secure_root) == actual,
          "input path changed during open");
  if (spec_.identity.inode == 0 && spec_.identity.device == 0)
    spec_.identity = opened_identity(file_);
  check_unchanged();
  projection_ = projection;
  for (size_t i : projection_)
    require(i < spec_.columns.size(), "invalid projected file column");
  line_ = 1;
  record_ = 0;
  if (spec_.options.format == "parquet") {
    parquet_.reset(new Parquet(file_, spec_, projection));
    return;
  }
  unsigned char bom[3];
  size_t n = fread(bom, 1, 3, file_);
  if (n != 3 || bom[0] != 0xef || bom[1] != 0xbb || bom[2] != 0xbf)
    rewind(file_);
  if (spec_.options.format == "csv" && spec_.options.header) {
    std::vector<Cell> header;
    csv_record(header);
  }
}
void Reader::rescan() { open(projection_); }
bool Reader::next(std::vector<Cell> &row) {
  check_unchanged();
  bool have = false;
  if (parquet_)
    have = parquet_->next(row);
  else if (spec_.options.format == "csv") {
    have = csv_record(row);
    if (have)
      require(row.size() == spec_.columns.size(),
              "CSV field count mismatch in " + spec_.path + " at record " +
                  std::to_string(record_));
  } else {
    std::vector<std::pair<std::string, Cell>> obj;
    have = json_record(obj);
    if (have) {
      row.assign(spec_.columns.size(), Cell());
      for (auto &p : obj) {
        auto it =
            std::find_if(spec_.columns.begin(), spec_.columns.end(),
                         [&](const Column &c) { return c.name == p.first; });
        require(it != spec_.columns.end(), "JSON schema changed");
        row[it - spec_.columns.begin()] = std::move(p.second);
      }
    }
  }
  check_unchanged();
  if (!have)
    return false;
  if (!parquet_ && !projection_.empty()) {
    std::vector<Cell> selected;
    for (size_t i : projection_)
      selected.push_back(std::move(row[i]));
    row = std::move(selected);
  }
  return true;
}
size_t Reader::next_batch(size_t capacity,
                          std::vector<std::vector<Cell>> &batch) {
  require(capacity > 0 && capacity <= 4096, "invalid file scan batch size");
  batch.clear();
  size_t bytes = 0;
  std::vector<Cell> r;
  while (batch.size() < capacity && (batch.empty() || bytes < 1024 * 1024) &&
         next(r)) {
    for (auto &c : r)
      bytes += c.text.size();
    require(bytes <= MAX_RECORD_BYTES + 1024 * 1024,
            "file scan batch resource limit exceeded");
    batch.push_back(std::move(r));
  }
  return batch.size();
}

Spec bind(const std::string &path, const std::string &root,
          const Options &options) {
  validate_options(options);
  Spec s;
  s.options = options;
  s.secure_root = root;
  if (options.format == "glob") {
    s.path = path;
    s.columns.push_back({"file", Type::VARCHAR, false});
    s.rows = discover(path, root).size();
    return s;
  }
  s.path = secure_path(path, root);
  s.identity = file_identity(s.path);
  if (options.format == "parquet") {
    Reader r(s);
    r.open();
    r.check_unchanged();
    return r.parquet_->schema();
  }
  auto merge_cell = [](Column &column, const Cell &cell) {
    const Type type = infer_type(cell);
    column.type = widen(column.type, type);
    column.nullable |= cell.null;
    if (type == Type::DATETIME) {
      const auto point = cell.text.find('.', 19);
      const int scale =
          point == std::string::npos ? 0 : int(cell.text.size() - point - 1);
      column.scale = std::max(column.scale, scale);
    }
  };
  Reader r(s);
  r.open();
  rewind(r.file_);
  unsigned char bom[3];
  size_t n = fread(bom, 1, 3, r.file_);
  if (n != 3 || bom[0] != 0xef || bom[1] != 0xbb || bom[2] != 0xbf)
    rewind(r.file_);
  if (options.format == "csv") {
    std::vector<Cell> row;
    bool first = true;
    std::set<std::string> names;
    while (r.csv_record(row)) {
      r.check_unchanged();
      if (first) {
        for (size_t i = 0; i < row.size(); ++i) {
          std::string name = options.header && !row[i].text.empty()
                                 ? row[i].text
                                 : "column" + std::to_string(i + 1),
                      base = name;
          for (unsigned k = 2; !names.insert(lower(name)).second; ++k)
            name = base + "__" + std::to_string(k);
          s.columns.push_back({name, Type::NIL, false});
        }
        first = false;
        for (const auto &c : s.columns)
          require(c.name.size() <= 1024,
                  "CSV column name exceeds resource limit");
        if (options.header)
          continue;
      }
      require(row.size() == s.columns.size(), "CSV field count mismatch in " +
                                                  s.path + " at record " +
                                                  std::to_string(r.record_));
      for (size_t i = 0; i < row.size(); ++i) {
        merge_cell(s.columns[i], row[i]);
      }
      ++s.rows;
    }
  } else {
    std::vector<std::pair<std::string, Cell>> obj;
    while (r.json_record(obj)) {
      r.check_unchanged();
      std::set<std::string> present;
      for (auto &p : obj) {
        require(p.first.size() <= 1024,
                "JSON column name exceeds resource limit");
        present.insert(p.first);
        auto it =
            std::find_if(s.columns.begin(), s.columns.end(),
                         [&](const Column &c) { return c.name == p.first; });
        if (it == s.columns.end()) {
          require(s.columns.size() < MAX_COLUMNS, "JSON column limit exceeded");
          for (auto &c : s.columns)
            require(lower(c.name) != lower(p.first),
                    "ambiguous JSON field name");
          s.columns.push_back({p.first, Type::NIL, s.rows > 0});
          merge_cell(s.columns.back(), p.second);
        } else {
          merge_cell(*it, p.second);
        }
      }
      for (auto &c : s.columns)
        if (!present.count(c.name))
          c.nullable = true;
      ++s.rows;
    }
  }
  require(!s.columns.empty(), "cannot infer schema from empty file: " + s.path);
  for (auto &column : s.columns) {
    if (column.type == Type::NIL)
      column.nullable = true;
    if (column.type != Type::DATETIME)
      column.scale = 0;
  }
  r.check_unchanged();
  return s;
}

CsvWriter::CsvWriter(const std::string &path, const std::string &root,
                     Options options)
    : root_(root), options_(std::move(options)) {
  try {
    validate_options(options_);
    local_path_only(path);
    fs::path requested = fs::u8path(path);
    require(!requested.filename().empty() && requested.filename() != "." &&
                requested.filename() != ".." &&
                path.find('\0') == std::string::npos,
            "invalid export path");
    fs::path parent = requested.parent_path();
    if (parent.empty())
      parent = ".";
    parent = fs::u8path(secure_path(path_text(parent), root));
    target_name_ = path_text(requested.filename());
    target_ = path_text(parent / requested.filename());
    require(!fs::exists(fs::u8path(target_)) &&
                !fs::is_symlink(fs::u8path(target_)),
            "export target already exists");
#ifdef _WIN32
    // Hold every directory in the canonical path without FILE_SHARE_DELETE.
    // A concurrent junction swap or directory rename cannot redirect creation.
    directory_handles_.reserve(256);
    fs::path current;
    for (const auto &component : parent) {
      current /= component;
      if (current == parent.root_name())
        continue;
      require(directory_handles_.size() < 256, "export directory depth limit");
      HANDLE handle = CreateFileW(
          current.c_str(), FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      require(handle != INVALID_HANDLE_VALUE, "cannot lock export directory");
      directory_handles_.push_back(reinterpret_cast<intptr_t>(handle));
      BY_HANDLE_FILE_INFORMATION info{};
      require(GetFileInformationByHandle(handle, &info) != 0 &&
                  (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                  !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
              "export directory changed");
    }
    require(secure_path(path_text(parent), root) == path_text(parent),
            "export directory changed");
#else
    directory_fd_ = ::open(path_text(parent).c_str(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    require(directory_fd_ >= 0, "cannot open export directory");
#ifdef __linux__
    std::vector<char> resolved(32768);
    ssize_t length =
        readlink(("/proc/self/fd/" + std::to_string(directory_fd_)).c_str(),
                 resolved.data(), resolved.size());
    require(length > 0 && size_t(length) < resolved.size(),
            "cannot resolve export directory");
    require(secure_path(std::string(resolved.data(), length), root) ==
                path_text(parent),
            "export directory changed");
#endif
#endif
    static std::atomic<uint64_t> counter{0};
    for (unsigned attempt = 0; attempt < 100 && file_ == nullptr; ++attempt) {
      temporary_name_ =
          target_name_ + ".file-sql-" + std::to_string(++counter) + ".tmp";
      temporary_ = path_text(parent / fs::u8path(temporary_name_));
#ifdef _WIN32
      temporary_native_ = fs::u8path(temporary_).native();
      int fd = _wopen(temporary_native_.c_str(),
                      _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                      _S_IREAD | _S_IWRITE);
#else
      int fd =
          openat(directory_fd_, temporary_name_.c_str(),
                 O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
#endif
      if (fd >= 0) {
        temporary_created_ = true;
#ifdef _WIN32
        file_ = _fdopen(fd, "wb");
        if (file_ == nullptr)
          _close(fd);
#else
        file_ = fdopen(fd, "wb");
        if (file_ == nullptr)
          close(fd);
#endif
        require(file_ != nullptr, "cannot open export stream");
      } else
        require(errno == EEXIST, "cannot create export temporary file");
    }
    require(file_ != nullptr, "cannot create export temporary file");
  } catch (...) {
    cleanup();
    throw;
  }
}
void CsvWriter::cleanup() noexcept {
  if (file_ != nullptr) {
    fclose(file_);
    file_ = nullptr;
  }
  if (!committed_ && temporary_created_) {
#ifdef _WIN32
    DeleteFileW(temporary_native_.c_str());
#else
    unlinkat(directory_fd_, temporary_name_.c_str(), 0);
#endif
  }
#ifdef _WIN32
  for (intptr_t handle : directory_handles_)
    CloseHandle(reinterpret_cast<HANDLE>(handle));
  directory_handles_.clear();
#else
  if (directory_fd_ >= 0) {
    close(directory_fd_);
    directory_fd_ = -1;
  }
#endif
}
CsvWriter::~CsvWriter() { cleanup(); }
void CsvWriter::write(const std::vector<Cell> &row) {
  require(file_ != nullptr && !committed_, "invalid export state");
  require(row.size() <= MAX_COLUMNS, "export column limit exceeded");
  std::string out;
  auto append = [&](const std::string &text) {
    require(text.size() <= MAX_RECORD_BYTES - out.size(),
            "export record exceeds resource limit");
    out += text;
  };
  auto character = [&](char value) {
    require(out.size() < MAX_RECORD_BYTES,
            "export record exceeds resource limit");
    out += value;
  };
  for (size_t i = 0; i < row.size(); ++i) {
    if (i)
      character(options_.delimiter);
    const Cell &cell = row[i];
    if (cell.null) {
      append(options_.nullstr);
      continue;
    }
    require(cell.text.size() <= MAX_RECORD_BYTES - out.size(),
            "export record exceeds resource limit");
    require(valid_utf8(cell.text), "CSV export text is not UTF-8");
    bool quote =
        cell.text.empty() || cell.text == options_.nullstr ||
        cell.text.find_first_of(std::string() + options_.delimiter +
                                options_.quote + "\r\n") != std::string::npos;
    if (quote)
      character(options_.quote);
    for (char value : cell.text) {
      if (quote &&
          (value == options_.quote ||
           (value == options_.escape && options_.escape != options_.quote)))
        character(options_.escape);
      character(value);
    }
    if (quote)
      character(options_.quote);
  }
  character('\n');
  require(fwrite(out.data(), 1, out.size(), file_) == out.size(),
          "CSV export write failed");
}
void CsvWriter::commit() {
  require(file_ != nullptr && !committed_, "invalid export state");
  require(fflush(file_) == 0, "CSV export flush failed");
#ifdef _WIN32
  require(_commit(_fileno(file_)) == 0, "CSV export sync failed");
#else
  require(fsync(fileno(file_)) == 0, "CSV export sync failed");
#endif
  int result = fclose(file_);
  file_ = nullptr;
  require(result == 0, "CSV export close failed");
  secure_path(path_text(fs::u8path(target_).parent_path()), root_);
#if defined(__linux__)
  // The opened directory can be renamed independently of its original path.
  // Recheck the descriptor before publishing through it.
  std::vector<char> resolved(32768);
  const ssize_t length =
      readlink(("/proc/self/fd/" + std::to_string(directory_fd_)).c_str(),
               resolved.data(), resolved.size());
  require(length > 0 && size_t(length) < resolved.size(),
          "cannot resolve export directory");
  require(secure_path(std::string(resolved.data(), length), root_) ==
              path_text(fs::u8path(target_).parent_path()),
          "export directory changed");
#endif
#ifdef _WIN32
  require(MoveFileExW(fs::u8path(temporary_).c_str(),
                      fs::u8path(target_).c_str(), MOVEFILE_WRITE_THROUGH) != 0,
          "CSV target already exists or publish failed");
#else
  require(linkat(directory_fd_, temporary_name_.c_str(), directory_fd_,
                 target_name_.c_str(), 0) == 0,
          "CSV target already exists or publish failed");
  if (unlinkat(directory_fd_, temporary_name_.c_str(), 0) != 0 ||
      fsync(directory_fd_) != 0) {
    unlinkat(directory_fd_, target_name_.c_str(), 0);
    throw Error("CSV export publish/cleanup failed");
  }
#endif
  committed_ = true;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
