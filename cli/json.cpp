// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "json.hpp"

#include <array>
#include <cstdio>

namespace entl::cli {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void append_code_point(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7Fu) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else if (code_point <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  }
}

}  // namespace

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8u);
  std::size_t index = 0;
  while (index < text.size()) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x80u) {
      switch (byte) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\b': out.append("\\b"); break;
        case '\f': out.append("\\f"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
          if (byte < 0x20u) {
            out.append("\\u00");
            out.push_back(kHexDigits[(byte >> 4) & 0x0Fu]);
            out.push_back(kHexDigits[byte & 0x0Fu]);
          } else {
            out.push_back(static_cast<char>(byte));
          }
          break;
      }
      ++index;
      continue;
    }
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if ((byte & 0xE0u) == 0xC0u) {
      extra = 1;
      code_point = byte & 0x1Fu;
    } else if ((byte & 0xF0u) == 0xE0u) {
      extra = 2;
      code_point = byte & 0x0Fu;
    } else if ((byte & 0xF8u) == 0xF0u) {
      extra = 3;
      code_point = byte & 0x07u;
    } else {
      out.append("\\ufffd");
      ++index;
      continue;
    }
    if (index + extra >= text.size()) {
      out.append("\\ufffd");
      break;
    }
    bool valid = true;
    for (std::size_t offset = 1; offset <= extra; ++offset) {
      const auto next = static_cast<unsigned char>(text[index + offset]);
      if ((next & 0xC0u) != 0x80u) {
        valid = false;
        break;
      }
      code_point = (code_point << 6) | (next & 0x3Fu);
    }
    if (!valid) {
      out.append("\\ufffd");
      ++index;
      continue;
    }
    // Escape the characters that JSON forbids raw and the two Unicode line
    // separators that are legal but hostile inside JavaScript string literals.
    if (code_point == 0x2028u || code_point == 0x2029u) {
      out.append("\\u202");
      out.push_back(code_point == 0x2028u ? '8' : '9');
    } else {
      append_code_point(out, code_point);
    }
    index += extra + 1u;
  }
  return out;
}

void JsonWriter::separate() {
  if (!first_.empty()) {
    if (!first_.back()) {
      out_.push_back(',');
    } else {
      first_.back() = false;
    }
  }
}

void JsonWriter::push(std::string_view text) {
  separate();
  out_.append(text);
}

JsonWriter& JsonWriter::begin_object() {
  push("{");
  first_.push_back(true);
  return *this;
}

JsonWriter& JsonWriter::end_object() {
  if (!first_.empty()) {
    first_.pop_back();
  }
  out_.push_back('}');
  return *this;
}

JsonWriter& JsonWriter::begin_array() {
  push("[");
  first_.push_back(true);
  return *this;
}

JsonWriter& JsonWriter::end_array() {
  if (!first_.empty()) {
    first_.pop_back();
  }
  out_.push_back(']');
  return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
  separate();
  if (!first_.empty()) {
    first_.back() = true;
  }
  out_.push_back('"');
  out_.append(json_escape(name));
  out_.append("\":");
  return *this;
}

JsonWriter& JsonWriter::value(std::string_view text) {
  push("\"");
  out_.append(json_escape(text));
  out_.push_back('"');
  return *this;
}

JsonWriter& JsonWriter::value(bool flag) {
  push(flag ? "true" : "false");
  return *this;
}

JsonWriter& JsonWriter::value(long long number) {
  push(std::to_string(number));
  return *this;
}

JsonWriter& JsonWriter::value(unsigned long long number) {
  push(std::to_string(number));
  return *this;
}

JsonWriter& JsonWriter::value(double number) {
  if (!(number == number) || number > 1.0e308 || number < -1.0e308) {
    return value(0.0f);
  }
  std::array<char, 64> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(), "%.6f", number);
  if (written <= 0) {
    return value(0);
  }
  push(std::string_view(buffer.data(), static_cast<std::size_t>(written)));
  return *this;
}

JsonWriter& JsonWriter::null_value() {
  push("null");
  return *this;
}

}  // namespace entl::cli
