// Resource Entitlement — the entitlement record and its lifecycle.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_ENTITLEMENT_HPP
#define RESOURCE_ENTITLEMENT_ENTITLEMENT_HPP

#include <optional>
#include <string>
#include <vector>

#include "resource_entitlement/binding.hpp"
#include "resource_entitlement/priority.hpp"
#include "resource_entitlement/quantity.hpp"
#include "resource_entitlement/scope.hpp"
#include "resource_entitlement/strong.hpp"
#include "resource_entitlement/time.hpp"

namespace entl {

/// Durable lifecycle state. A record in kActive state still only confers
/// authority when its time window contains the decision instant, it has
/// remaining quantity, and its authority binding is not stale; see
/// evaluate_liveness().
enum class EntitlementState : std::uint8_t {
  kActive = 1,
  kSuspended = 2,
  kRevoked = 3,
  kExpired = 4,
  kSuperseded = 5,
};

[[nodiscard]] bool is_valid_entitlement_state(EntitlementState state) noexcept;
[[nodiscard]] bool is_terminal_state(EntitlementState state) noexcept;
[[nodiscard]] const char* entitlement_state_name(EntitlementState state) noexcept;
[[nodiscard]] std::optional<EntitlementState> entitlement_state_from_name(std::string_view name) noexcept;

/// How a record came into existence relative to its lineage.
enum class LineageRelation : std::uint8_t {
  kRoot = 1,
  kSplitFrom = 2,
  kDelegatedFrom = 3,
  kMovedFrom = 4,
  kReissuedFrom = 5,
  kMergedFrom = 6,
};

[[nodiscard]] bool is_valid_lineage_relation(LineageRelation relation) noexcept;
[[nodiscard]] const char* lineage_relation_name(LineageRelation relation) noexcept;

/// Why a record reached a terminal state. kNotApplicable is the explicit
/// "no terminal reason" value.
enum class TerminalReason : std::uint8_t {
  kNotApplicable = 0,
  kOperatorRevocation = 1,
  kPolicyRevocation = 2,
  kExpiredByTime = 3,
  kSupersededByReissue = 4,
  kSupersededByMerge = 5,
  kParentRevoked = 6,
};

[[nodiscard]] bool is_valid_terminal_reason(TerminalReason reason) noexcept;
[[nodiscard]] const char* terminal_reason_name(TerminalReason reason) noexcept;

/// A historical change of holder. Lineage preserves who held authority before,
/// without implying that the previous holder retains any.
struct HolderChange {
  TenantId from{};
  TenantId to{};
  Timestamp at{};
  Sequence commit_seq{};
  ActorId actor{};
  std::string note{};

  void encode(CanonicalWriter& writer) const;
  static HolderChange decode(CanonicalReader& reader);
};

/// A generation-bound grant of authority to consume within a bounded scope.
struct Entitlement {
  EntitlementId id{};
  EntitlementId root{};
  LineageRelation relation{LineageRelation::kRoot};
  std::vector<EntitlementId> sources{};
  Revision revision{};
  MintOrdinal mint_ordinal{};
  EntitlementState state{EntitlementState::kActive};
  TenantId holder{};
  TenantId origin_holder{};
  ServiceId service{};
  ServiceClassId service_class{};
  ResourceScope scope{};
  Unit unit{Unit::kCount};
  Quantity granted{};
  Quantity remaining{};
  Quantity delegated_out{};
  Priority priority{};
  Timestamp effective_from{};
  Timestamp expires_at{};
  Timestamp created_at{};
  Timestamp updated_at{};
  ActorId created_by{};
  AuthorityBinding binding{};
  FenceMask fence{};
  Sha256Digest request_digest{};
  std::string note{};
  std::optional<Timestamp> suspended_at{};
  std::optional<Timestamp> revoked_at{};
  TerminalReason terminal_reason{TerminalReason::kNotApplicable};
  std::vector<HolderChange> holder_history{};
  Sequence last_commit_seq{};

  [[nodiscard]] bool is_terminal() const noexcept { return is_terminal_state(state); }
  [[nodiscard]] bool is_exhausted() const noexcept { return remaining.is_zero(); }
  [[nodiscard]] bool has_admission_evidence() const noexcept {
    return !binding.admission_decision_digest.is_zero();
  }

  /// Half-open window [effective_from, expires_at). At exactly expires_at the
  /// entitlement is already expired.
  [[nodiscard]] bool window_contains(const Timestamp& instant) const noexcept {
    return !(instant < effective_from) && instant < expires_at;
  }

  void encode(CanonicalWriter& writer) const;
  static Entitlement decode(CanonicalReader& reader);
};

/// Derived liveness of a record at a decision instant under a snapshot. This is
/// a computed view, never a stored field.
struct LivenessView {
  bool live{false};
  ErrorCode primary{ErrorCode::kOk};
  bool stale{false};
  std::vector<ErrorCode> faults{};
  std::string explanation{};
};

/// Deterministic liveness evaluation. Fault ordering matches the verification
/// precedence documented in README.
[[nodiscard]] LivenessView evaluate_liveness(const Entitlement& entitlement,
                                             const AuthoritySnapshot& snapshot,
                                             const Timestamp& now);

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_ENTITLEMENT_HPP
