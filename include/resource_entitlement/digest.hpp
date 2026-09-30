// Resource Entitlement — SHA-256 / CRC-32C content digests.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_DIGEST_HPP
#define RESOURCE_ENTITLEMENT_DIGEST_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace entl {

/// A 32-byte SHA-256 content digest.
///
/// A default-constructed digest is the all-zero digest. The all-zero digest is
/// never produced by hashing and is used only as an explicit "not supplied"
/// marker; every place that requires real evidence rejects it.
class Sha256Digest {
public:
  static constexpr std::size_t kSize = 32;

  constexpr Sha256Digest() noexcept = default;

  /// Requires exactly kSize bytes; returns nullopt otherwise.
  [[nodiscard]] static std::optional<Sha256Digest> from_bytes(std::span<const std::uint8_t> bytes) noexcept;

  /// Requires exactly 64 hexadecimal characters (either case).
  [[nodiscard]] static std::optional<Sha256Digest> from_hex(std::string_view hex) noexcept;

  [[nodiscard]] const std::array<std::uint8_t, kSize>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::span<const std::uint8_t> span() const noexcept { return bytes_; }

  /// 64 lowercase hexadecimal characters.
  [[nodiscard]] std::string to_hex() const;

  [[nodiscard]] bool is_zero() const noexcept;

  friend bool operator==(const Sha256Digest&, const Sha256Digest&) noexcept = default;
  friend std::strong_ordering operator<=>(const Sha256Digest&, const Sha256Digest&) noexcept = default;

private:
  std::array<std::uint8_t, kSize> bytes_{};
};

/// Streaming SHA-256 (FIPS 180-4). Deterministic, allocation-free.
class Sha256 {
public:
  Sha256() noexcept;

  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view text) noexcept;

  /// Finalizes and returns the digest. Idempotent: calling finish() twice
  /// returns the same digest, and update() after finish() is ignored.
  [[nodiscard]] Sha256Digest finish() noexcept;

  [[nodiscard]] static Sha256Digest hash(std::span<const std::uint8_t> data) noexcept;
  [[nodiscard]] static Sha256Digest hash(std::string_view text) noexcept;

private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_{0};
  std::size_t buffered_{0};
  bool finalized_{false};
  Sha256Digest result_{};
};

/// CRC-32C (Castagnoli, reflected, polynomial 0x1EDC6F41, init/xorout 0xFFFFFFFF).
[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept;

/// Lowercase hexadecimal encoding of arbitrary bytes.
[[nodiscard]] std::string bytes_to_hex(std::span<const std::uint8_t> bytes);

/// Strict hexadecimal decoding. Returns nullopt for an odd number of digits,
/// for any non-hexadecimal character, or when \p expected_size is non-zero and
/// the decoded length differs from it.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> hex_to_bytes(std::string_view hex,
                                                                   std::size_t expected_size);

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_DIGEST_HPP
