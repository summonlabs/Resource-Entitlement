// Resource Entitlement — strong identity, counter, and digest aliases.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_STRONG_HPP
#define RESOURCE_ENTITLEMENT_STRONG_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "resource_entitlement/canonical.hpp"
#include "resource_entitlement/digest.hpp"
#include "resource_entitlement/error.hpp"
#include "resource_entitlement/text.hpp"

namespace entl {

/// Validated identifier storage shared by every strongly typed identifier.
class IdentifierValue {
public:
  IdentifierValue() noexcept = default;

  [[nodiscard]] static Result<IdentifierValue> parse(std::string_view text);

  [[nodiscard]] bool is_set() const noexcept { return size_ != 0u; }
  [[nodiscard]] std::string_view view() const noexcept { return std::string_view(data_.data(), size_); }
  [[nodiscard]] std::string str() const { return std::string(view()); }

  friend bool operator==(const IdentifierValue& a, const IdentifierValue& b) noexcept { return a.view() == b.view(); }
  friend std::strong_ordering operator<=>(const IdentifierValue& a, const IdentifierValue& b) noexcept { return a.view() <=> b.view(); }

  void encode(CanonicalWriter& writer) const;
  static IdentifierValue decode(CanonicalReader& reader);

private:
  std::array<char, kMaxIdentifierBytes> data_{};
  std::uint8_t size_{0};
};

/// Strongly typed identifier. Distinct tags make tenants, services, facilities,
/// resource types, scopes, and service classes mutually incompatible.
template <class Tag>
class StrongIdentifier {
public:
  StrongIdentifier() noexcept = default;

  [[nodiscard]] static Result<StrongIdentifier> parse(std::string_view text) {
    auto parsed = IdentifierValue::parse(text);
    if (!parsed) {
      return parsed.error();
    }
    return StrongIdentifier(parsed.value());
  }

  [[nodiscard]] bool is_set() const noexcept { return value_.is_set(); }
  [[nodiscard]] std::string_view view() const noexcept { return value_.view(); }
  [[nodiscard]] std::string str() const { return value_.str(); }
  [[nodiscard]] const IdentifierValue& value() const noexcept { return value_; }

  friend bool operator==(const StrongIdentifier&, const StrongIdentifier&) noexcept = default;
  friend std::strong_ordering operator<=>(const StrongIdentifier&, const StrongIdentifier&) noexcept = default;

  void encode(CanonicalWriter& writer) const { value_.encode(writer); }
  static StrongIdentifier decode(CanonicalReader& reader) {
    StrongIdentifier result;
    result.value_ = IdentifierValue::decode(reader);
    return result;
  }

private:
  explicit StrongIdentifier(IdentifierValue value) noexcept : value_(value) {}
  IdentifierValue value_{};
};

struct TenantIdTag {};
struct ServiceIdTag {};
struct FacilityIdTag {};
struct ResourceTypeIdTag {};
struct ResourceScopeIdTag {};
struct ServiceClassIdTag {};
struct PolicyRefIdTag {};
struct ActorIdTag {};
struct EvidenceRefIdTag {};

using TenantId = StrongIdentifier<TenantIdTag>;
using ServiceId = StrongIdentifier<ServiceIdTag>;
using FacilityId = StrongIdentifier<FacilityIdTag>;
using ResourceTypeId = StrongIdentifier<ResourceTypeIdTag>;
using ResourceScopeId = StrongIdentifier<ResourceScopeIdTag>;
using ServiceClassId = StrongIdentifier<ServiceClassIdTag>;
using PolicyRefId = StrongIdentifier<PolicyRefIdTag>;
using ActorId = StrongIdentifier<ActorIdTag>;
using EvidenceRefId = StrongIdentifier<EvidenceRefIdTag>;

/// Monotonic, non-wrapping counter base. Successor computation is checked; an
/// exhausted counter reports nullopt rather than wrapping to zero.
template <class Tag>
class Counter {
public:
  using value_type = std::uint64_t;

  constexpr Counter() noexcept = default;

  [[nodiscard]] static constexpr Counter from_value(std::uint64_t value) noexcept { return Counter(value); }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0u; }
  [[nodiscard]] constexpr bool is_set() const noexcept { return value_ != 0u; }

  [[nodiscard]] constexpr std::optional<Counter> next() const noexcept {
    if (value_ == kMax) {
      return std::nullopt;
    }
    return Counter(value_ + 1u);
  }

  [[nodiscard]] static constexpr Counter max() noexcept { return Counter(kMax); }

  friend constexpr bool operator==(const Counter&, const Counter&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const Counter&, const Counter&) noexcept = default;

  void encode(CanonicalWriter& writer) const { writer.u64(value_); }
  static Counter decode(CanonicalReader& reader) { return Counter(reader.u64()); }

private:
  static constexpr std::uint64_t kMax = 18446744073709551615ull;
  explicit constexpr Counter(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_{0};
};

struct GenerationTag {};
struct EpochTag {};
struct RevisionTag {};
struct SequenceTag {};
struct MintTag {};

using Generation = Counter<GenerationTag>;
using Epoch = Counter<EpochTag>;
using Revision = Counter<RevisionTag>;
using Sequence = Counter<SequenceTag>;
using MintOrdinal = Counter<MintTag>;

/// 16-byte opaque identity. Values are derived, never stored as free text, and
/// never reused.
template <class Tag>
class Id16 {
public:
  static constexpr std::size_t kSize = 16;

  Id16() noexcept = default;

  /// Derives a deterministic identity from a caller-supplied seed by hashing it
  /// with SHA-256 and keeping the first 16 bytes.
  [[nodiscard]] static Id16 derive(std::span<const std::uint8_t> seed) noexcept;
  [[nodiscard]] static Id16 derive(std::string_view seed) noexcept;

  [[nodiscard]] static std::optional<Id16> from_bytes(std::span<const std::uint8_t> bytes) noexcept;
  [[nodiscard]] static std::optional<Id16> from_hex(std::string_view hex) noexcept;

  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] const std::array<std::uint8_t, kSize>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::span<const std::uint8_t> span() const noexcept { return bytes_; }
  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] bool is_set() const noexcept { return !is_zero(); }

  friend bool operator==(const Id16&, const Id16&) noexcept = default;
  friend std::strong_ordering operator<=>(const Id16&, const Id16&) noexcept = default;

  void encode(CanonicalWriter& writer) const { writer.fixed(bytes_); }
  static Id16 decode(CanonicalReader& reader) {
    Id16 result;
    const auto raw = reader.fixed(kSize);
    if (raw.size() == kSize) {
      for (std::size_t i = 0; i < kSize; ++i) {
        result.bytes_[i] = raw[i];
      }
    }
    return result;
  }

private:
  std::array<std::uint8_t, kSize> bytes_{};
};

struct RequestIdTag {};
struct EntitlementIdTag {};
struct ReservationIdTag {};
struct TokenIdTag {};

using RequestId = Id16<RequestIdTag>;
using EntitlementId = Id16<EntitlementIdTag>;
using ReservationId = Id16<ReservationIdTag>;
using TokenId = Id16<TokenIdTag>;

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_STRONG_HPP
