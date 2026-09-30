// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "resource_entitlement/text.hpp"

#include <array>
#include <cstdint>

namespace entl {
namespace {

constexpr std::array<std::string_view, 22> kReservedDeviceNames = {
    "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
    "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

[[nodiscard]] constexpr bool is_identifier_start(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] constexpr bool is_identifier_body(char c) noexcept {
  return is_identifier_start(c) || c == '.' || c == ':' || c == '-';
}

enum class Utf8Scan { kOk, kInvalid, kControl };

[[nodiscard]] Utf8Scan scan_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80u) {
      if (lead < 0x20u || lead == 0x7Fu) {
        return Utf8Scan::kControl;
      }
      ++index;
      continue;
    }
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if ((lead & 0xE0u) == 0xC0u) {
      extra = 1;
      code_point = lead & 0x1Fu;
      minimum = 0x80u;
    } else if ((lead & 0xF0u) == 0xE0u) {
      extra = 2;
      code_point = lead & 0x0Fu;
      minimum = 0x800u;
    } else if ((lead & 0xF8u) == 0xF0u) {
      extra = 3;
      code_point = lead & 0x07u;
      minimum = 0x10000u;
    } else {
      return Utf8Scan::kInvalid;
    }
    if (text.size() - index <= extra) {
      return Utf8Scan::kInvalid;
    }
    for (std::size_t offset = 1; offset <= extra; ++offset) {
      const auto next = static_cast<unsigned char>(text[index + offset]);
      if ((next & 0xC0u) != 0x80u) {
        return Utf8Scan::kInvalid;
      }
      code_point = (code_point << 6) | (next & 0x3Fu);
    }
    if (code_point < minimum || code_point > 0x10FFFFu) {
      return Utf8Scan::kInvalid;
    }
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
      return Utf8Scan::kInvalid;
    }
    if (code_point >= 0x80u && code_point <= 0x9Fu) {
      return Utf8Scan::kControl;
    }
    index += extra + 1u;
  }
  return Utf8Scan::kOk;
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept { return scan_utf8(text) != Utf8Scan::kInvalid; }

bool is_valid_text(std::string_view text) noexcept {
  if (text.size() > kMaxTextBytes) {
    return false;
  }
  return scan_utf8(text) == Utf8Scan::kOk;
}

bool is_windows_reserved_name(std::string_view text) noexcept {
  const std::size_t dot = text.find('.');
  const std::string_view stem = (dot == std::string_view::npos) ? text : text.substr(0, dot);
  if (stem.empty()) {
    return false;
  }
  for (const std::string_view reserved : kReservedDeviceNames) {
    if (equals_ascii_ignore_case(stem, reserved)) {
      return true;
    }
  }
  return false;
}

bool is_valid_identifier(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxIdentifierBytes) {
    return false;
  }
  if (!is_identifier_start(text.front())) {
    return false;
  }
  for (std::size_t i = 1; i < text.size(); ++i) {
    if (!is_identifier_body(text[i])) {
      return false;
    }
  }
  if (text.back() == '.') {
    return false;
  }
  if (text.find("..") != std::string_view::npos) {
    return false;
  }
  if (is_windows_reserved_name(text)) {
    return false;
  }
  return true;
}

std::string to_lower_ascii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

bool equals_ascii_ignore_case(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') {
      ca = static_cast<char>(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = static_cast<char>(cb - 'A' + 'a');
    }
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

std::string_view truncate_utf8(std::string_view text, std::size_t limit) noexcept {
  if (text.size() <= limit) {
    return text;
  }
  std::size_t cut = limit;
  while (cut > 0u && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
    --cut;
  }
  return text.substr(0, cut);
}

}  // namespace entl
