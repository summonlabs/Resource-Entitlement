// Resource Entitlement — units and bounded quantities.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_QUANTITY_HPP
#define RESOURCE_ENTITLEMENT_QUANTITY_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>

#include "resource_entitlement/canonical.hpp"

namespace entl {

/// Integral units in which entitlement authority is expressed. The unit is part
/// of every quantity so that arithmetic across incompatible units is refused
/// rather than silently reinterpreted.
enum class Unit : std::uint8_t {
  kCount = 1,
  kBytes = 2,
  kMebibytes = 3,
  kGibibytes = 4,
  kMebibytesPerSecond = 5,
  kMillicores = 6,
  kIops = 7,
  kSessions = 8,
  kImages = 9,
  kWatts = 10,
  kMilliwatts = 11,
};

[[nodiscard]] bool is_valid_unit(Unit unit) noexcept;
[[nodiscard]] const char* unit_name(Unit unit) noexcept;
[[nodiscard]] std::optional<Unit> unit_from_name(std::string_view name) noexcept;

/// A non-negative integral amount in a specific unit.
///
/// Zero is a legitimate value (fully consumed authority). Absence is modelled
/// by std::optional<Quantity>, never by a magic zero.
class Quantity {
public:
  constexpr Quantity() noexcept = default;

  /// Returns nullopt when the unit is not a valid enumerator.
  [[nodiscard]] static std::optional<Quantity> make(Unit unit, std::uint64_t units) noexcept;

  /// Strict decimal parse: 1..20 ASCII digits, no sign, no separators, no
  /// leading '+' or whitespace. Values above UINT64_MAX are rejected.
  [[nodiscard]] static std::optional<Quantity> parse(Unit unit, std::string_view digits) noexcept;

  [[nodiscard]] constexpr Unit unit() const noexcept { return unit_; }
  [[nodiscard]] constexpr std::uint64_t units() const noexcept { return units_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return units_ == 0u; }

  /// Checked addition; nullopt on unit mismatch or overflow.
  [[nodiscard]] static std::optional<Quantity> checked_add(const Quantity& a, const Quantity& b) noexcept;

  /// Checked subtraction; nullopt on unit mismatch or underflow.
  [[nodiscard]] static std::optional<Quantity> checked_sub(const Quantity& a, const Quantity& b) noexcept;

  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(const Quantity& a, const Quantity& b) noexcept {
    return a.units_ == b.units_ && a.unit_ == b.unit_;
  }
  friend constexpr std::strong_ordering operator<=>(const Quantity& a, const Quantity& b) noexcept {
    if (a.unit_ != b.unit_) {
      return a.unit_ <=> b.unit_;
    }
    return a.units_ <=> b.units_;
  }

  void encode(CanonicalWriter& writer) const;
  static Quantity decode(CanonicalReader& reader);

private:
  Unit unit_{Unit::kCount};
  std::uint64_t units_{0};
};

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_QUANTITY_HPP
