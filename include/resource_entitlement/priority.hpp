// Resource Entitlement — advisory priority ordering.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_PRIORITY_HPP
#define RESOURCE_ENTITLEMENT_PRIORITY_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>

#include "resource_entitlement/canonical.hpp"

namespace entl {

/// Priority class. Higher enumerator values are more important.
///
/// Priority in this repository is recorded authority metadata and an ordering
/// input only. It never preempts, revokes, suspends, or shortens another
/// entitlement. Any preemption behaviour belongs to a repository that owns
/// admission and scheduling decisions, which this repository does not.
enum class PriorityClass : std::uint8_t {
  kBackground = 1,
  kBestEffort = 2,
  kStandard = 3,
  kElevated = 4,
  kCritical = 5,
  kGuaranteed = 6,
};

[[nodiscard]] bool is_valid_priority_class(PriorityClass klass) noexcept;
[[nodiscard]] const char* priority_class_name(PriorityClass klass) noexcept;
[[nodiscard]] std::optional<PriorityClass> priority_class_from_name(std::string_view name) noexcept;

/// A class plus an in-class rank. Rank 0 is the most favourable rank in its
/// class. The pair gives a deterministic total order once ties are broken by
/// entitlement identity.
struct Priority {
  PriorityClass klass{PriorityClass::kStandard};
  std::uint16_t rank{0};

  friend constexpr bool operator==(const Priority&, const Priority&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const Priority& a, const Priority& b) noexcept {
    if (a.klass != b.klass) {
      return a.klass <=> b.klass;
    }
    return a.rank <=> b.rank;
  }

  void encode(CanonicalWriter& writer) const;
  static Priority decode(CanonicalReader& reader);
};

/// Ordering comparator used by Store::order_for_consumption().
/// Returns a negative value when p a should be considered before p b.
[[nodiscard]] int compare_priority(const Priority& a, const Priority& b) noexcept;

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_PRIORITY_HPP
