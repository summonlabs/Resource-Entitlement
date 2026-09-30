// Resource Entitlement — resource scope binding.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_SCOPE_HPP
#define RESOURCE_ENTITLEMENT_SCOPE_HPP

#include "resource_entitlement/canonical.hpp"
#include "resource_entitlement/strong.hpp"

namespace entl {

/// The exact physical/service scope an entitlement authorizes consumption
/// against. All three components must match on every verification and on every
/// transfer, split, or merge; the scope is opaque to this repository and is
/// defined by Facility Topology and Resource Envelope.
struct ResourceScope {
  FacilityId facility{};
  ResourceTypeId resource_type{};
  ResourceScopeId scope{};

  [[nodiscard]] bool is_set() const noexcept {
    return facility.is_set() && resource_type.is_set() && scope.is_set();
  }

  friend bool operator==(const ResourceScope&, const ResourceScope&) noexcept = default;
  friend std::strong_ordering operator<=>(const ResourceScope&, const ResourceScope&) noexcept = default;

  void encode(CanonicalWriter& writer) const {
    facility.encode(writer);
    resource_type.encode(writer);
    scope.encode(writer);
  }

  static ResourceScope decode(CanonicalReader& reader) {
    ResourceScope result;
    result.facility = FacilityId::decode(reader);
    result.resource_type = ResourceTypeId::decode(reader);
    result.scope = ResourceScopeId::decode(reader);
    return result;
  }
};

/// True when every component matches.
[[nodiscard]] bool scope_matches(const ResourceScope& a, const ResourceScope& b) noexcept;

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_SCOPE_HPP
