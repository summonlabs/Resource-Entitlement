// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "resource_entitlement/canonical.hpp"

#include <cstring>

#include "resource_entitlement/text.hpp"

namespace entl {

// ---------------------------------------------------------------------------
// CanonicalWriter
// ---------------------------------------------------------------------------

void CanonicalWriter::u8(std::uint8_t value) { data_.push_back(value); }

void CanonicalWriter::u16(std::uint16_t value) {
  data_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  data_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void CanonicalWriter::u32(std::uint32_t value) {
  for (unsigned shift = 0; shift < 32u; shift += 8u) {
    data_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void CanonicalWriter::u64(std::uint64_t value) {
  for (unsigned shift = 0; shift < 64u; shift += 8u) {
    data_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void CanonicalWriter::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void CanonicalWriter::boolean(bool value) { data_.push_back(value ? 1u : 0u); }

void CanonicalWriter::presence(bool present) { data_.push_back(present ? 1u : 0u); }

void CanonicalWriter::fixed(std::span<const std::uint8_t> data) {
  data_.insert(data_.end(), data.begin(), data.end());
}

void CanonicalWriter::bytes(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxCanonicalFieldBytes || data.size() > 0xFFFFFFFFu) {
    valid_ = false;
    return;
  }
  u32(static_cast<std::uint32_t>(data.size()));
  fixed(data);
}

void CanonicalWriter::text(std::string_view value) {
  bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

Sha256Digest CanonicalWriter::digest() const noexcept {
  return Sha256::hash(std::span<const std::uint8_t>(data_.data(), data_.size()));
}

std::string CanonicalWriter::to_hex() const {
  return bytes_to_hex(std::span<const std::uint8_t>(data_.data(), data_.size()));
}

// ---------------------------------------------------------------------------
// CanonicalReader
// ---------------------------------------------------------------------------

std::uint8_t CanonicalReader::u8() {
  if (failed_ || pos_ + 1u > data_.size()) {
    fail();
    return 0u;
  }
  return data_[pos_++];
}

std::uint16_t CanonicalReader::u16() {
  if (failed_ || pos_ + 2u > data_.size()) {
    fail();
    return 0u;
  }
  const auto value = static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[pos_]) |
                                                 (static_cast<std::uint16_t>(data_[pos_ + 1u]) << 8));
  pos_ += 2u;
  return value;
}

std::uint32_t CanonicalReader::u32() {
  if (failed_ || pos_ + 4u > data_.size()) {
    fail();
    return 0u;
  }
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4u; ++i) {
    value |= static_cast<std::uint32_t>(data_[pos_ + i]) << (i * 8u);
  }
  pos_ += 4u;
  return value;
}

std::uint64_t CanonicalReader::u64() {
  if (failed_ || pos_ + 8u > data_.size()) {
    fail();
    return 0u;
  }
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8u; ++i) {
    value |= static_cast<std::uint64_t>(data_[pos_ + i]) << (i * 8u);
  }
  pos_ += 8u;
  return value;
}

std::int64_t CanonicalReader::i64() { return static_cast<std::int64_t>(u64()); }

bool CanonicalReader::boolean() {
  const std::uint8_t value = u8();
  if (failed_) {
    return false;
  }
  if (value > 1u) {
    fail();
    return false;
  }
  return value == 1u;
}

bool CanonicalReader::presence() {
  const std::uint8_t value = u8();
  if (failed_) {
    return false;
  }
  if (value > 1u) {
    fail();
    return false;
  }
  return value == 1u;
}

std::span<const std::uint8_t> CanonicalReader::fixed(std::size_t count) {
  if (failed_ || pos_ + count > data_.size()) {
    fail();
    return {};
  }
  const auto result = data_.subspan(pos_, count);
  pos_ += count;
  return result;
}

std::span<const std::uint8_t> CanonicalReader::bytes(std::size_t max_length) {
  if (failed_) {
    return {};
  }
  const std::uint32_t length = u32();
  if (failed_) {
    return {};
  }
  if (static_cast<std::size_t>(length) > max_length || static_cast<std::size_t>(length) > kMaxCanonicalFieldBytes) {
    fail();
    return {};
  }
  return fixed(static_cast<std::size_t>(length));
}

std::string CanonicalReader::text(std::size_t max_length) {
  const std::span<const std::uint8_t> raw = bytes(max_length);
  if (failed_) {
    return {};
  }
  const std::string_view view(reinterpret_cast<const char*>(raw.data()), raw.size());
  if (!is_valid_text(view)) {
    fail();
    return {};
  }
  return std::string(view);
}

Result<void> CanonicalReader::finish() const {
  if (failed_) {
    return Error(ErrorCode::kInvalidEncoding, "canonical decoding failed before the end of the payload");
  }
  if (!exhausted()) {
    return Error(ErrorCode::kTrailingBytes, "canonical payload has unconsumed trailing bytes",
                 std::to_string(remaining()) + " byte(s) remain");
  }
  return {};
}

}  // namespace entl
