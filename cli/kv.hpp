// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Strict command-line and request-file parsing. Everything is bounded and
// unknown input is rejected rather than ignored.

#ifndef RESOURCE_ENTITLEMENT_CLI_KV_HPP
#define RESOURCE_ENTITLEMENT_CLI_KV_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "resource_entitlement/error.hpp"

namespace entl::cli {

inline constexpr std::size_t kMaxRequestBytes = 64u * 1024u;
inline constexpr std::size_t kMaxArgumentBytes = 4096u;

/// Parsed command arguments: "--name value" pairs and "--flag" switches.
class Args {
public:
  [[nodiscard]] static Result<Args> parse(std::vector<std::string> arguments);

  [[nodiscard]] bool has(std::string_view name) const;
  [[nodiscard]] std::optional<std::string> optional(std::string_view name) const;
  [[nodiscard]] Result<std::string> require(std::string_view name) const;
  [[nodiscard]] Result<std::uint64_t> require_u64(std::string_view name) const;
  [[nodiscard]] Result<std::uint64_t> optional_u64(std::string_view name, std::uint64_t fallback) const;
  [[nodiscard]] Result<bool> optional_bool(std::string_view name, bool fallback) const;

  /// Fails when an argument was supplied that the command does not understand.
  [[nodiscard]] Result<void> reject_unknown(const std::vector<std::string>& allowed) const;

  [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& entries() const noexcept {
    return entries_;
  }

private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

/// A parsed key=value request document.
class KvDocument {
public:
  [[nodiscard]] static Result<KvDocument> parse(std::string_view text,
                                                const std::vector<std::string>& allowed_keys,
                                                const std::vector<std::string>& required_keys);

  [[nodiscard]] bool has(std::string_view key) const;
  [[nodiscard]] std::optional<std::string> optional(std::string_view key) const;
  [[nodiscard]] Result<std::string> require(std::string_view key) const;
  [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& entries() const noexcept {
    return entries_;
  }

private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

/// Reads a whole file (or standard input when \p path is "-") with a bound.
[[nodiscard]] Result<std::string> read_text_source(const std::string& path);

/// Renders bytes as lowercase hexadecimal.
[[nodiscard]] std::string to_hex(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] Result<std::vector<std::uint8_t>> from_hex(std::string_view text);

}  // namespace entl::cli

#endif  // RESOURCE_ENTITLEMENT_CLI_KV_HPP
