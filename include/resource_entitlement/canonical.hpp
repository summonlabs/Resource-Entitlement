// Resource Entitlement — canonical, bounded, exact binary serialization.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_CANONICAL_HPP
#define RESOURCE_ENTITLEMENT_CANONICAL_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "resource_entitlement/digest.hpp"
#include "resource_entitlement/error.hpp"

namespace entl {

/// Hard bound on any canonically encoded object accepted by the library.
inline constexpr std::size_t kMaxCanonicalBytes = 8u * 1024u * 1024u;

/// Hard bound on a single length-prefixed field.
inline constexpr std::size_t kMaxCanonicalFieldBytes = 1u * 1024u * 1024u;

/// Deterministic encoder.
///
/// Encoding rules (fixed for store format version 1):
///   * every integer is little-endian, fixed width, no padding;
///   * optional values are written as a 1-byte presence tag (0 or 1) followed
///     by the value only when present;
///   * booleans are a single byte, strictly 0 or 1;
///   * byte strings and text are a 4-byte little-endian length followed by the
///     exact bytes; text is not NUL-terminated and is never normalized;
///   * enums are written at their declared width;
///   * there is no map ordering freedom: callers emit fields in declaration
///     order, so equal values always produce byte-identical encodings.
class CanonicalWriter {
public:
  CanonicalWriter() = default;

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);

  /// Writes a 1-byte presence tag.
  void presence(bool present);

  /// Writes raw bytes with no length prefix.
  void fixed(std::span<const std::uint8_t> data);

  /// Writes a 4-byte length prefix followed by the bytes.
  void bytes(std::span<const std::uint8_t> data);

  /// Same as bytes(); the content is expected to be valid UTF-8.
  void text(std::string_view value);

  [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
  [[nodiscard]] std::span<const std::uint8_t> span() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

  /// False when an oversized field was written; the encoding must then be
  /// discarded rather than persisted.
  [[nodiscard]] bool valid() const noexcept { return valid_; }

  [[nodiscard]] Sha256Digest digest() const noexcept;

  [[nodiscard]] std::string to_hex() const;

private:
  std::vector<std::uint8_t> data_;
  bool valid_{true};
};

/// Strict decoder. All read helpers return a zero/empty value and set a sticky
/// failure flag when the input is malformed; callers check ok() (or finish()).
class CanonicalReader {
public:
  explicit CanonicalReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  [[nodiscard]] bool ok() const noexcept { return !failed_; }
  [[nodiscard]] bool exhausted() const noexcept { return pos_ == data_.size(); }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }

  std::uint8_t u8();
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  std::int64_t i64();
  bool boolean();

  /// Reads a presence tag; marks the reader failed for any byte other than 0/1.
  bool presence();

  std::span<const std::uint8_t> fixed(std::size_t count);
  std::span<const std::uint8_t> bytes(std::size_t max_length);
  std::string text(std::size_t max_length);

  void fail() noexcept { failed_ = true; }

  /// Succeeds only when nothing failed and every byte was consumed.
  [[nodiscard]] Result<void> finish() const;

private:
  std::span<const std::uint8_t> data_;
  std::size_t pos_{0};
  bool failed_{false};
};

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_CANONICAL_HPP
