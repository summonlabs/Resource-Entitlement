// Resource Entitlement — verification requests and decisions.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_DECISION_HPP
#define RESOURCE_ENTITLEMENT_DECISION_HPP

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "resource_entitlement/binding.hpp"
#include "resource_entitlement/quantity.hpp"
#include "resource_entitlement/scope.hpp"
#include "resource_entitlement/strong.hpp"
#include "resource_entitlement/time.hpp"

namespace entl {

/// The caller's claim about who is asking and for what. When supplied, every
/// field must match the entitlement exactly.
struct ExpectedContext {
  TenantId holder{};
  ServiceId service{};
  ServiceClassId service_class{};
  ResourceScope scope{};
  Unit unit{Unit::kCount};

  void encode(CanonicalWriter& writer) const;
  static ExpectedContext decode(CanonicalReader& reader);
};

/// A fully attributable verification answer. authorized is true only when
/// primary is kOk; every other observed fault is retained in secondary_faults
/// in the fixed precedence order.
struct VerificationDecision {
  bool authorized{false};
  ErrorCode primary{ErrorCode::kOk};
  std::string explanation{};
  EntitlementId id{};
  Revision revision{};
  Epoch control_epoch{};
  Sequence observed_commit_seq{};
  Timestamp decision_time{};
  std::optional<Quantity> requested{};
  Quantity remaining{};
  Sha256Digest binding_digest{};
  Sha256Digest token_digest{};
  bool stale{false};
  std::vector<ErrorCode> secondary_faults{};

  [[nodiscard]] std::string to_string() const;
};

struct VerifyRequest {
  Timestamp now{};
  EntitlementId id{};
  std::optional<ExpectedContext> expected{};
  std::optional<Quantity> requested{};
};

struct VerifyTokenRequest {
  Timestamp now{};
  std::span<const std::uint8_t> token{};
  std::optional<ExpectedContext> expected{};
  std::optional<Quantity> requested{};
};

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_DECISION_HPP
