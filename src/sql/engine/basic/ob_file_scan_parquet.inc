/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
// Included only by ob_file_scan_utils.cpp, inside namespace file_sql.
struct PQValue {
  int64_t number = 0;
  std::string text;
  std::vector<PQValue> list;
  std::map<int, PQValue> fields;
  const PQValue &get(int id) const {
    static const PQValue empty;
    auto it = fields.find(id);
    return it == fields.end() ? empty : it->second;
  }
};
namespace {
struct PQCompact {
  const std::string &data;
  size_t pos = 0;
  size_t nodes = 0;
  uint8_t byte() {
    require(pos < data.size(), "truncated Parquet metadata/page header");
    return uint8_t(data[pos++]);
  }
  uint64_t varint() {
    uint64_t n = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      uint8_t b = byte();
      require(shift != 63 || b <= 1, "Parquet varint overflow");
      n |= uint64_t(b & 127) << shift;
      if (!(b & 128))
        return n;
    }
    throw Error("invalid Parquet varint");
  }
  int64_t integer() {
    uint64_t n = varint();
    return int64_t(n >> 1) ^ -int64_t(n & 1);
  }
  PQValue value(unsigned type, unsigned depth = 0) {
    require(depth <= 24 && ++nodes <= MAX_SCAN_MEMORY / sizeof(PQValue),
            "Parquet metadata resource limit");
    PQValue v;
    if (type == 1 || type == 2)
      v.number = type == 1;
    else if (type == 3)
      v.number = int8_t(byte());
    else if (type >= 4 && type <= 6)
      v.number = integer();
    else if (type == 7) {
      require(pos + 8 <= data.size(), "truncated Parquet double");
      pos += 8;
    } else if (type == 8) {
      uint64_t n = varint();
      require(n <= MAX_METADATA_BYTES && n <= data.size() - pos,
              "Parquet string limit");
      v.text = data.substr(pos, size_t(n));
      pos += n;
    } else if (type == 9 || type == 10) {
      unsigned h = byte(), t = h & 15;
      uint64_t n = h >> 4;
      if (n == 15)
        n = varint();
      require(n <= MAX_METADATA_BYTES / 8, "Parquet list limit");
      for (uint64_t i = 0; i < n; ++i) {
        if (t == 1 || t == 2) {
          unsigned b = byte();
          require(b == 1 || b == 2, "invalid compact boolean");
          v.list.push_back(value(b, depth + 1));
        } else
          v.list.push_back(value(t, depth + 1));
      }
    } else if (type == 11) {
      uint64_t n = varint();
      require(n <= MAX_METADATA_BYTES / 16, "Parquet map limit");
      if (n) {
        unsigned h = byte();
        for (uint64_t i = 0; i < n; ++i) {
          value(h >> 4, depth + 1);
          value(h & 15, depth + 1);
        }
      }
    } else if (type == 12) {
      int id = 0;
      for (;;) {
        unsigned h = byte();
        if (!h)
          break;
        unsigned delta = h >> 4;
        id = delta ? id + int(delta) : int(integer());
        require(id > 0 && v.fields.size() < 1024, "invalid Parquet field id");
        require(!v.fields.count(id), "duplicate Parquet metadata field");
        v.fields.emplace(id, value(h & 15, depth + 1));
      }
    } else
      throw Error("invalid Parquet compact type");
    return v;
  }
};
uint32_t pq_u32(const char *p) {
  return uint32_t(uint8_t(p[0])) | (uint32_t(uint8_t(p[1])) << 8) |
         (uint32_t(uint8_t(p[2])) << 16) | (uint32_t(uint8_t(p[3])) << 24);
}
uint64_t pq_u64(const char *p) {
  return uint64_t(pq_u32(p)) | (uint64_t(pq_u32(p + 4)) << 32);
}
uint32_t pq_crc32(const std::string &data) {
  uint32_t crc = 0xffffffffU;
  for (unsigned char byte : data) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
void pq_seek(FILE *f, uint64_t off) {
#ifdef _WIN32
  require(_fseeki64(f, static_cast<int64_t>(off), SEEK_SET) == 0,
          "Parquet seek failed");
#else
  require(fseeko(f, static_cast<off_t>(off), SEEK_SET) == 0,
          "Parquet seek failed");
#endif
}
std::string pq_read(FILE *f, uint64_t offset, size_t n) {
  require(n <= MAX_METADATA_BYTES, "Parquet page/footer exceeds memory limit");
  pq_seek(f, offset);
  std::string s(n, '\0');
  require(n == 0 || fread(s.data(), 1, n, f) == n, "truncated Parquet file");
  return s;
}
std::string pq_snappy(const std::string &s, size_t expected) {
  PQCompact p{s};
  uint64_t length = p.varint();
  require(length == expected && length <= MAX_METADATA_BYTES,
          "invalid Snappy output length");
  std::string out;
  out.reserve(expected);
  while (p.pos < s.size()) {
    unsigned tag = p.byte(), kind = tag & 3;
    size_t n = 0, offset = 0;
    if (kind == 0) {
      n = tag >> 2;
      if (n < 60)
        ++n;
      else {
        unsigned bytes = unsigned(n) - 59;
        require(bytes <= 4, "invalid Snappy literal");
        n = 0;
        for (unsigned i = 0; i < bytes; ++i)
          n |= size_t(p.byte()) << (8 * i);
        ++n;
      }
      require(n <= s.size() - p.pos && n <= expected - out.size(),
              "Snappy literal overflow");
      out.append(s, p.pos, n);
      p.pos += n;
    } else {
      if (kind == 1) {
        n = 4 + ((tag >> 2) & 7);
        offset = ((tag & 0xe0) << 3) | p.byte();
      } else {
        n = 1 + (tag >> 2);
        unsigned bytes = kind == 2 ? 2 : 4;
        for (unsigned i = 0; i < bytes; ++i)
          offset |= size_t(p.byte()) << (8 * i);
      }
      require(offset && offset <= out.size() && n <= expected - out.size(),
              "invalid Snappy copy");
      for (size_t i = 0; i < n; ++i)
        out += out[out.size() - offset];
    }
  }
  require(out.size() == expected, "truncated Snappy block");
  return out;
}
std::vector<uint32_t> pq_hybrid(const std::string &data, size_t &pos,
                                size_t end, unsigned width, size_t count) {
  require(width <= 32 && end <= data.size() && count <= MAX_METADATA_BYTES / 8,
          "Parquet RLE resource limit");
  std::vector<uint32_t> out;
  out.reserve(count);
  while (out.size() < count) {
    require(pos < end, "truncated Parquet levels/indices");
    uint64_t h = 0;
    unsigned shift = 0;
    for (;;) {
      require(pos < end && shift < 64, "invalid Parquet RLE header");
      unsigned b = uint8_t(data[pos++]);
      require(shift != 63 || b <= 1, "Parquet RLE header overflow");
      h |= uint64_t(b & 127) << shift;
      if (!(b & 128))
        break;
      shift += 7;
    }
    require(h > 0, "empty Parquet RLE run");
    if (!(h & 1)) {
      uint64_t n = h >> 1;
      require(n <= count - out.size(), "Parquet RLE count overflow");
      uint32_t v = 0;
      for (unsigned j = 0; j < (width + 7) / 8; ++j) {
        require(pos < end, "truncated Parquet RLE value");
        v |= uint32_t(uint8_t(data[pos++])) << (8 * j);
      }
      require(width == 32 || v < (uint64_t(1) << width),
              "Parquet RLE value overflow");
      out.insert(out.end(), size_t(n), v);
    } else {
      uint64_t groups = h >> 1;
      require(groups && groups <= MAX_METADATA_BYTES &&
                  groups * width <= end - pos,
              "truncated Parquet bit-packed run");
      uint64_t n = groups * 8;
      require(n <= count - out.size() + 7, "Parquet bit-pack count overflow");
      for (uint64_t j = 0; j < n && out.size() < count; ++j) {
        uint32_t v = 0;
        for (unsigned bit = 0; bit < width; ++bit) {
          size_t at = size_t(j * width + bit);
          v |= ((uint8_t(data[pos + at / 8]) >> (at % 8)) & 1U) << bit;
        }
        out.push_back(v);
      }
      pos += size_t(groups * width);
    }
  }
  return out;
}
std::string pq_day(int64_t z) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = unsigned(z - era * 146097),
           yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = int64_t(yoe) + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100),
           mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
  int m = int(mp) + (mp < 10 ? 3 : -9);
  y += m <= 2;
  require(y >= 1 && y <= 9999, "Parquet date out of range");
  char b[32];
  snprintf(b, sizeof(b), "%04lld-%02d-%02u", static_cast<long long>(y), m, d);
  return b;
}
std::string pq_timestamp(int64_t value, int64_t divisor) {
  int64_t seconds = value / divisor, rem = value % divisor;
  if (rem < 0) {
    --seconds;
    rem += divisor;
  }
  int64_t days = seconds / 86400, s = seconds % 86400;
  if (s < 0) {
    --days;
    s += 86400;
  }
  char b[40];
  snprintf(b, sizeof(b), " %02lld:%02lld:%02lld.%06lld",
           static_cast<long long>(s / 3600),
           static_cast<long long>((s % 3600) / 60),
           static_cast<long long>(s % 60),
           static_cast<long long>(rem * (1000000 / divisor)));
  return pq_day(days) + b;
}
} // namespace

struct Reader::Parquet {
  struct Field {
    int physical = 0, length = 0, converted = -1;
    bool optional = false;
  };
  struct Chunk {
    uint64_t start = 0, end = 0, offset = 0;
    int codec = 0;
    size_t page_index = 0;
    std::vector<Cell> dictionary, page;
  };
  FILE *file;
  Spec bound;
  PQValue footer;
  std::vector<Field> fields;
  std::vector<size_t> projection;
  std::vector<Chunk> chunks;
  size_t group = 0;
  uint64_t row = 0, group_rows = 0;
  uint64_t footer_start = 0;
  bool loaded = false;
  Parquet(FILE *f, const Spec &s, const std::vector<size_t> &p)
      : file(f), bound(s), projection(p) {
    require(s.identity.size >= 12, "invalid Parquet file size");
    auto tail = pq_read(f, s.identity.size - 8, 8);
    require(tail.substr(4) == "PAR1" && pq_read(f, 0, 4) == "PAR1",
            "invalid Parquet magic");
    uint32_t n = pq_u32(tail.data());
    require(n <= MAX_METADATA_BYTES && uint64_t(n) + 12 <= s.identity.size,
            "invalid Parquet footer size");
    footer_start = s.identity.size - 8 - n;
    auto meta = pq_read(f, footer_start, n);
    PQCompact c{meta};
    footer = c.value(12);
    require(c.pos == meta.size(), "trailing Parquet footer data");
    require(footer.fields.count(1) && footer.fields.count(2) &&
                footer.fields.count(3) && footer.fields.count(4) &&
                footer.get(1).number >= 1 && footer.get(1).number <= 2,
            "invalid or incomplete Parquet file metadata");
    const auto &schema = footer.get(2).list;
    require(schema.size() > 1 && schema.size() <= MAX_COLUMNS + 1 &&
                schema[0].get(5).number == int64_t(schema.size() - 1),
            "Parquet requires a flat schema");
    std::vector<Column> columns;
    for (size_t i = 1; i < schema.size(); ++i) {
      const auto &v = schema[i];
      require(!v.fields.count(5) && v.get(3).number != 2,
              "nested/repeated Parquet fields are not supported");
      Field field;
      field.physical = int(v.get(1).number);
      field.length = int(v.get(2).number);
      field.converted = v.fields.count(6) ? int(v.get(6).number) : -1;
      field.optional = v.get(3).number == 1;
      require(v.fields.count(1) && field.physical >= 0 && field.physical <= 7,
              "unsupported Parquet physical type");
      Type t = field.physical == 0                          ? Type::BOOLEAN
               : field.physical == 1 || field.physical == 2 ? Type::BIGINT
               : field.physical == 3                        ? Type::DATETIME
               : field.physical == 4 || field.physical == 5 ? Type::DOUBLE
                                                            : Type::VARCHAR;
      if (field.converted == 6)
        t = Type::DATE;
      else if (field.converted == 9 || field.converted == 10)
        t = Type::DATETIME;
      else
        require(field.converted < 0 || field.converted == 0 ||
                    field.converted == 11 || field.converted == 12 ||
                    field.converted == 13 || field.converted == 15 ||
                    field.converted == 16 || field.converted == 17 ||
                    field.converted == 18 || field.converted == 14,
                "unsupported Parquet logical type");
      if (field.converted == 14)
        t = Type::VARCHAR;
      if (v.fields.count(10)) {
        const auto &logical = v.get(10);
        if (logical.fields.count(8)) {
          const auto &ts = logical.get(8);
          const auto &unit = ts.get(2);
          field.converted = unit.fields.count(1)   ? 9
                            : unit.fields.count(2) ? 10
                                                   : -2;
          require(field.converted != -2,
                  "nanosecond Parquet timestamps are not supported");
          t = Type::DATETIME;
        } else if (logical.fields.count(6)) {
          field.converted = 6;
          t = Type::DATE;
        } else if (logical.fields.count(11)) {
          field.converted = -3;
          t = Type::NIL;
        } else if (logical.fields.count(10)) {
          const auto &integer = logical.get(10);
          int64_t width = integer.get(1).number;
          require(width == 8 || width == 16 || width == 32 || width == 64,
                  "invalid Parquet integer width");
          if (!integer.get(2).number) {
            field.converted = width == 64   ? 14
                              : width == 32 ? 13
                              : width == 16 ? 12
                                            : 11;
            if (width == 64)
              t = Type::VARCHAR;
          }
        } else
          require(logical.fields.count(1), "unsupported Parquet logical type");
      }
      require((field.converted != 6 || field.physical == 1) &&
                  ((field.converted != 9 && field.converted != 10) ||
                   field.physical == 2) &&
                  (field.converted != 0 || field.physical == 6) &&
                  ((field.converted != 14 && field.converted != 18) ||
                   field.physical == 2) &&
                  (!(field.converted >= 11 && field.converted <= 17 &&
                     field.converted != 14) ||
                   field.physical == 1),
              "Parquet logical/physical type mismatch");
      require(!v.get(4).text.empty() && v.get(4).text.size() <= 1024 &&
                  valid_utf8(v.get(4).text),
              "invalid Parquet column name");
      for (auto &col : columns)
        require(lower(col.name) != lower(v.get(4).text),
                "duplicate Parquet column name");
      const int scale =
          t == Type::DATETIME ? (field.converted == 9 ? 3 : 6) : 0;
      columns.push_back({v.get(4).text, t, field.optional, scale});
      fields.push_back(field);
    }
    bound.columns = columns;
    require(footer.get(3).number >= 0, "invalid Parquet row count");
    bound.rows = uint64_t(footer.get(3).number);
    uint64_t total = 0;
    for (const auto &g : footer.get(4).list) {
      require(g.get(3).number >= 0 && g.get(1).list.size() == columns.size(),
              "invalid Parquet row group");
      require(uint64_t(g.get(3).number) <= bound.rows - total,
              "Parquet row group count overflow");
      total += uint64_t(g.get(3).number);
    }
    require(total == bound.rows, "Parquet row group count mismatch");
    if (!s.columns.empty()) {
      require(s.columns.size() == columns.size(), "Parquet schema changed");
      for (size_t i = 0; i < columns.size(); ++i)
        require(s.columns[i].name == columns[i].name &&
                    s.columns[i].type == columns[i].type,
                "Parquet schema changed");
    }
    if (projection.empty())
      for (size_t i = 0; i < fields.size(); ++i)
        projection.push_back(i);
  }
  Spec schema() const { return bound; }
  void check_memory() const {
    size_t bytes = 0;
    for (const auto &chunk : chunks) {
      bytes +=
          (chunk.page.capacity() + chunk.dictionary.capacity()) * sizeof(Cell);
      for (const auto &cell : chunk.page)
        bytes += cell.text.capacity();
      for (const auto &cell : chunk.dictionary)
        bytes += cell.text.capacity();
      require(bytes <= MAX_SCAN_MEMORY, "Parquet scan memory limit exceeded");
    }
  }
  bool load_group() {
    while (group < footer.get(4).list.size()) {
      const auto &g = footer.get(4).list[group++];
      group_rows = uint64_t(g.get(3).number);
      row = 0;
      chunks.clear();
      if (group_rows == 0)
        continue;
      for (size_t col : projection) {
        require(col < fields.size(), "invalid Parquet projection");
        const auto &c = g.get(1).list[col];
        require(c.get(1).text.empty() && c.fields.count(3),
                "external Parquet column chunks are not supported");
        const auto &m = c.get(3);
        require(m.get(1).number == fields[col].physical &&
                    m.get(5).number == int64_t(group_rows) &&
                    m.get(3).list.size() == 1 &&
                    m.get(3).list[0].text == bound.columns[col].name,
                "invalid Parquet column metadata");
        Chunk chunk;
        chunk.codec = int(m.get(4).number);
        require(chunk.codec == 0 || chunk.codec == 1,
                "Parquet supports UNCOMPRESSED and SNAPPY codecs");
        int64_t start = m.get(9).number;
        if (m.fields.count(11))
          start = std::min(start, m.get(11).number);
        require(start >= 4 && m.get(7).number >= 0 &&
                    uint64_t(start) + uint64_t(m.get(7).number) <= footer_start,
                "invalid Parquet column chunk bounds");
        chunk.start = chunk.offset = uint64_t(start);
        chunk.end = chunk.start + uint64_t(m.get(7).number);
        chunks.push_back(std::move(chunk));
      }
      loaded = true;
      if (group_rows)
        return true;
    }
    return false;
  }
  std::vector<Cell> plain(const std::string &s, size_t pos, size_t count,
                          const Field &f) {
    require(count <= MAX_METADATA_BYTES / 16,
            "Parquet page value limit exceeded");
    std::vector<Cell> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      std::string value;
      auto need = [&](size_t n) {
        require(n <= s.size() - pos, "truncated Parquet PLAIN value");
      };
      if (f.physical == 0) {
        require(pos + i / 8 < s.size(), "truncated Parquet boolean");
        value = (uint8_t(s[pos + i / 8]) & (1U << (i % 8))) ? "true" : "false";
      } else if (f.physical == 1) {
        need(4);
        int32_t n = int32_t(pq_u32(s.data() + pos));
        pos += 4;
        value = f.converted == 6 ? pq_day(n)
                : f.converted >= 11 && f.converted <= 13
                    ? std::to_string(uint32_t(n))
                    : std::to_string(n);
      } else if (f.physical == 2) {
        need(8);
        int64_t n = int64_t(pq_u64(s.data() + pos));
        pos += 8;
        value = f.converted == 9 || f.converted == 10
                    ? pq_timestamp(n, f.converted == 9 ? 1000 : 1000000)
                : f.converted == 14 ? std::to_string(uint64_t(n))
                                    : std::to_string(n);
      } else if (f.physical == 3) {
        need(12);
        int64_t nanos = int64_t(pq_u64(s.data() + pos));
        int64_t days = int64_t(pq_u32(s.data() + pos + 8)) - 2440588;
        pos += 12;
        require(nanos >= 0 && nanos < 86400000000000LL,
                "invalid Parquet INT96 time");
        // Restrict the Julian day before converting days to microseconds.
        require(days >= -719162 && days <= 2932896,
                "Parquet INT96 date out of range");
        value = pq_timestamp(days * 86400000000LL + nanos / 1000, 1000000);
      } else if (f.physical == 4 || f.physical == 5) {
        size_t n = f.physical == 4 ? 4 : 8;
        need(n);
        double d;
        if (n == 4) {
          uint32_t bits = pq_u32(s.data() + pos);
          float v;
          memcpy(&v, &bits, 4);
          d = v;
        } else {
          uint64_t bits = pq_u64(s.data() + pos);
          memcpy(&d, &bits, 8);
        }
        pos += n;
        require(std::isfinite(d), "nonfinite Parquet floating point value");
        std::ostringstream o;
        o.imbue(std::locale::classic());
        o << std::setprecision(std::numeric_limits<double>::max_digits10) << d;
        value = o.str();
      } else {
        size_t n;
        if (f.physical == 6) {
          need(4);
          n = pq_u32(s.data() + pos);
          pos += 4;
        } else {
          require(f.length > 0, "invalid Parquet fixed length");
          n = size_t(f.length);
        }
        need(n);
        require(n <= MAX_RECORD_BYTES, "Parquet string limit");
        value = s.substr(pos, n);
        pos += n;
        require(valid_utf8(value), "Parquet string is not UTF-8");
      }
      out.emplace_back(value);
    }
    return out;
  }
  void load_page(size_t idx) {
    Chunk &chunk = chunks[idx];
    const Field &field = fields[projection[idx]];
    for (;;) {
      require(chunk.offset < chunk.end, "truncated Parquet column chunk");
      std::string header =
          pq_read(file, chunk.offset,
                  size_t(std::min<uint64_t>(65536, chunk.end - chunk.offset)));
      PQCompact parser{header};
      PQValue h = parser.value(12);
      int64_t compressed = h.get(3).number, uncompressed = h.get(2).number;
      require(compressed >= 0 && uncompressed >= 0 &&
                  uint64_t(compressed) <= MAX_METADATA_BYTES &&
                  uint64_t(uncompressed) <= MAX_METADATA_BYTES &&
                  chunk.offset + parser.pos + uint64_t(compressed) <= chunk.end,
              "invalid Parquet page size");
      std::string data =
          pq_read(file, chunk.offset + parser.pos, size_t(compressed));
      if (h.fields.count(4))
        require(pq_crc32(data) == uint32_t(h.get(4).number),
                "Parquet page checksum mismatch");
      chunk.offset += parser.pos + uint64_t(compressed);
      int kind = int(h.get(1).number);
      bool v2 = kind == 3;
      const PQValue &info = h.get(kind == 2 ? 7 : v2 ? 8 : 5);
      size_t levels_size = 0;
      int64_t count = info.get(1).number;
      require(count >= 0 && count <= int64_t(MAX_METADATA_BYTES / 16),
              "invalid Parquet page count");
      if (v2) {
        require(info.get(5).number >= 0 && info.get(6).number == 0,
                "invalid Parquet V2 levels");
        levels_size = size_t(info.get(5).number);
        require(levels_size <= data.size() &&
                    levels_size <= size_t(uncompressed),
                "invalid V2 definition size");
        if (chunk.codec == 1 && (!info.fields.count(7) || info.get(7).number))
          data = data.substr(0, levels_size) +
                 pq_snappy(data.substr(levels_size),
                           size_t(uncompressed) - levels_size);
      } else if (chunk.codec == 1)
        data = pq_snappy(data, size_t(uncompressed));
      require(data.size() == size_t(uncompressed),
              "Parquet uncompressed size mismatch");
      if (kind == 2) {
        require(info.get(2).number == 0,
                "unsupported Parquet dictionary encoding");
        chunk.dictionary = plain(data, 0, size_t(count), field);
        check_memory();
        continue;
      }
      if (kind != 0 && kind != 3)
        continue;
      require(uint64_t(count) <= group_rows - row,
              "Parquet page row count overflow");
      require(v2 || info.get(3).number == 3,
              "unsupported Parquet definition encoding");
      size_t pos = 0;
      std::vector<uint32_t> defined(size_t(count), 1);
      if (field.optional) {
        if (!v2) {
          require(data.size() >= 4, "truncated Parquet levels");
          levels_size = pq_u32(data.data());
          pos = 4;
        }
        size_t end = pos + levels_size;
        require(end <= data.size(), "Parquet definition levels overflow");
        defined = pq_hybrid(data, pos, end, 1, size_t(count));
        pos = end;
      }
      size_t nonnull = 0;
      for (uint32_t x : defined) {
        require(x <= 1, "invalid Parquet definition level");
        nonnull += x;
      }
      require(field.converted != -3 || nonnull == 0,
              "nonnull Parquet NULL field");
      int encoding = int(info.get(v2 ? 4 : 2).number);
      std::vector<Cell> values;
      if (encoding == 0)
        values = plain(data, pos, nonnull, field);
      else if (encoding == 3 && field.physical == 0) {
        require(data.size() - pos >= 4, "truncated Parquet boolean RLE length");
        size_t end = pos + 4 + pq_u32(data.data() + pos);
        pos += 4;
        require(end <= data.size(), "invalid Parquet boolean RLE length");
        auto bits = pq_hybrid(data, pos, end, 1, nonnull);
        for (auto bit : bits)
          values.emplace_back(bit ? "true" : "false");
      } else if ((encoding == 2 || encoding == 8) && nonnull == 0) {
      } else if (encoding == 2 || encoding == 8) {
        require(pos < data.size() && !chunk.dictionary.empty(),
                "missing Parquet dictionary");
        unsigned width = uint8_t(data[pos++]);
        auto indices = pq_hybrid(data, pos, data.size(), width, nonnull);
        for (uint32_t id : indices) {
          require(id < chunk.dictionary.size(),
                  "Parquet dictionary index out of range");
          values.push_back(chunk.dictionary[id]);
        }
      } else
        throw Error("unsupported Parquet page encoding: " +
                    std::to_string(encoding));
      chunk.page.clear();
      chunk.page_index = 0;
      size_t value = 0;
      for (uint32_t x : defined)
        chunk.page.push_back(x ? std::move(values[value++]) : Cell());
      check_memory();
      if (!chunk.page.empty())
        return;
    }
  }
  bool next(std::vector<Cell> &out) {
    if (!loaded || row == group_rows) {
      if (!load_group())
        return false;
    }
    out.clear();
    size_t memory = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
      if (chunks[i].page_index == chunks[i].page.size())
        load_page(i);
      auto &c = chunks[i];
      out.push_back(c.page[c.page_index++]);
      memory += out.back().text.size();
      require(memory <= MAX_RECORD_BYTES, "Parquet output row limit exceeded");
    }
    ++row;
    return true;
  }
};
