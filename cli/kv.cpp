// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "kv.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

#include "resource_entitlement/digest.hpp"
#include "resource_entitlement/text.hpp"

namespace entl::cli {
namespace {

[[nodiscard]] bool is_valid_key(std::string_view key) {
  if (key.empty() || key.size() > 64u) {
    return false;
  }
  for (const char c : key) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
    ++begin;
  }
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

}  // namespace

Result<Args> Args::parse(std::vector<std::string> arguments) {
  Args args;
  std::size_t index = 0;
  while (index < arguments.size()) {
    const std::string& token = arguments[index];
    if (token.size() < 3u || token.rfind("--", 0) != 0) {
      return Error(ErrorCode::kMalformedInput, "unexpected argument; options must start with '--'", token);
    }
    const std::string name = token.substr(2);
    if (!is_valid_key(name)) {
      return Error(ErrorCode::kMalformedInput, "option name is not valid", token);
    }
    for (const auto& existing : args.entries_) {
      if (existing.first == name) {
        return Error(ErrorCode::kDuplicateField, "option supplied more than once", token);
      }
    }
    if (index + 1u < arguments.size() && arguments[index + 1u].rfind("--", 0) != 0) {
      args.entries_.emplace_back(name, arguments[index + 1u]);
      index += 2u;
      continue;
    }
    args.entries_.emplace_back(name, std::string());
    index += 1u;
  }
  return args;
}

bool Args::has(std::string_view name) const {
  return std::any_of(entries_.begin(), entries_.end(),
                     [name](const std::pair<std::string, std::string>& entry) { return entry.first == name; });
}

std::optional<std::string> Args::optional(std::string_view name) const {
  for (const auto& entry : entries_) {
    if (entry.first == name) {
      return entry.second;
    }
  }
  return std::nullopt;
}

Result<std::string> Args::require(std::string_view name) const {
  const auto found = optional(name);
  if (!found.has_value()) {
    return Error(ErrorCode::kMissingField, "missing required option", "--" + std::string(name));
  }
  if (found->empty()) {
    return Error(ErrorCode::kMissingField, "option requires a value", "--" + std::string(name));
  }
  return found.value();
}

Result<std::uint64_t> Args::require_u64(std::string_view name) const {
  auto text = require(name);
  if (!text) {
    return text.error();
  }
  if (text->empty() || text->size() > 20u) {
    return Error(ErrorCode::kInvalidArgument, "option is not a decimal integer", "--" + std::string(name));
  }
  std::uint64_t value = 0;
  for (const char c : text.value()) {
    if (c < '0' || c > '9') {
      return Error(ErrorCode::kInvalidArgument, "option is not a decimal integer", "--" + std::string(name));
    }
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > ((18446744073709551615ull - digit) / 10ull)) {
      return Error(ErrorCode::kInvalidArgument, "option overflows an unsigned 64-bit integer",
                   "--" + std::string(name));
    }
    value = (value * 10ull) + digit;
  }
  return value;
}

Result<std::uint64_t> Args::optional_u64(std::string_view name, std::uint64_t fallback) const {
  if (!has(name)) {
    return fallback;
  }
  return require_u64(name);
}

Result<bool> Args::optional_bool(std::string_view name, bool fallback) const {
  const auto found = optional(name);
  if (!found.has_value()) {
    return fallback;
  }
  if (found->empty() || found.value() == "true" || found.value() == "1" || found.value() == "yes") {
    return true;
  }
  if (found.value() == "false" || found.value() == "0" || found.value() == "no") {
    return false;
  }
  return Error(ErrorCode::kInvalidArgument, "flag must be true or false", "--" + std::string(name));
}

Result<void> Args::reject_unknown(const std::vector<std::string>& allowed) const {
  for (const auto& entry : entries_) {
    if (std::find(allowed.begin(), allowed.end(), entry.first) == allowed.end()) {
      return Error(ErrorCode::kUnexpectedField, "unknown option", "--" + entry.first);
    }
  }
  return {};
}

Result<KvDocument> KvDocument::parse(std::string_view text, const std::vector<std::string>& allowed_keys,
                                     const std::vector<std::string>& required_keys) {
  if (text.size() > kMaxRequestBytes) {
    return Error(ErrorCode::kTooLong, "request document exceeds the supported size bound");
  }
  if (!is_valid_utf8(text)) {
    return Error(ErrorCode::kInvalidText, "request document is not valid UTF-8");
  }
  KvDocument document;
  std::size_t offset = 0;
  std::size_t line_number = 0;
  while (offset <= text.size()) {
    const std::size_t end = text.find('\n', offset);
    const std::string_view line =
        (end == std::string_view::npos) ? text.substr(offset) : text.substr(offset, end - offset);
    offset = (end == std::string_view::npos) ? text.size() + 1u : end + 1u;
    ++line_number;
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    const std::size_t equals = trimmed.find('=');
    if (equals == std::string::npos) {
      return Error(ErrorCode::kMalformedInput, "request line is not key=value",
                   "line " + std::to_string(line_number));
    }
    const std::string key = trim(std::string_view(trimmed).substr(0, equals));
    const std::string value = trim(std::string_view(trimmed).substr(equals + 1u));
    if (!is_valid_key(key)) {
      return Error(ErrorCode::kMalformedInput, "request key is not valid",
                   "line " + std::to_string(line_number));
    }
    if (std::find(allowed_keys.begin(), allowed_keys.end(), key) == allowed_keys.end()) {
      return Error(ErrorCode::kUnexpectedField, "unknown request key", key);
    }
    for (const auto& entry : document.entries_) {
      if (entry.first == key) {
        return Error(ErrorCode::kDuplicateField, "request key supplied more than once", key);
      }
    }
    if (value.size() > kMaxRequestBytes) {
      return Error(ErrorCode::kTooLong, "request value exceeds the supported size bound", key);
    }
    document.entries_.emplace_back(key, value);
  }
  for (const std::string& key : required_keys) {
    if (!document.has(key)) {
      return Error(ErrorCode::kMissingField, "request is missing a required key", key);
    }
  }
  return document;
}

bool KvDocument::has(std::string_view key) const {
  return std::any_of(entries_.begin(), entries_.end(),
                     [key](const std::pair<std::string, std::string>& entry) { return entry.first == key; });
}

std::optional<std::string> KvDocument::optional(std::string_view key) const {
  for (const auto& entry : entries_) {
    if (entry.first == key) {
      return entry.second;
    }
  }
  return std::nullopt;
}

Result<std::string> KvDocument::require(std::string_view key) const {
  const auto found = optional(key);
  if (!found.has_value() || found->empty()) {
    return Error(ErrorCode::kMissingField, "request is missing a required key", std::string(key));
  }
  return found.value();
}

Result<std::string> read_text_source(const std::string& path) {
  if (path == "-") {
    std::ostringstream buffer;
    std::string line;
    std::size_t total = 0;
    while (std::getline(std::cin, line)) {
      total += line.size() + 1u;
      if (total > kMaxRequestBytes) {
        return Error(ErrorCode::kTooLong, "standard input exceeds the supported size bound");
      }
      buffer << line << '\n';
    }
    return buffer.str();
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Error(ErrorCode::kIoError, "cannot open request file", path);
  }
  std::string content;
  content.resize(kMaxRequestBytes + 1u);
  stream.read(content.data(), static_cast<std::streamsize>(content.size()));
  const auto read = static_cast<std::size_t>(stream.gcount());
  content.resize(read);
  if (read > kMaxRequestBytes) {
    return Error(ErrorCode::kTooLong, "request file exceeds the supported size bound", path);
  }
  return content;
}

std::string to_hex(const std::vector<std::uint8_t>& bytes) {
  return bytes_to_hex(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

Result<std::vector<std::uint8_t>> from_hex(std::string_view text) {
  if ((text.size() % 2u) != 0u || text.size() > 2u * kMaxRequestBytes) {
    return Error(ErrorCode::kInvalidDigest, "hexadecimal text has an invalid length");
  }
  auto decoded = hex_to_bytes(text, 0u);
  if (!decoded.has_value()) {
    return Error(ErrorCode::kInvalidDigest, "text is not valid hexadecimal");
  }
  return decoded.value();
}

}  // namespace entl::cli
