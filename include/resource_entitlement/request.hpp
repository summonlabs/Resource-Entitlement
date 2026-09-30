// Resource Entitlement — mutating requests and their recorded outcomes.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_REQUEST_HPP
#define RESOURCE_ENTITLEMENT_REQUEST_HPP

#include <optional>
#include <string>
#include <vector>

#include "resource_entitlement/binding.hpp"
#include "resource_entitlement/entitlement.hpp"
#include "resource_entitlement/priority.hpp"
#include "resource_entitlement/quantity.hpp"
#include "resource_entitlement/strong.hpp"
#include "resource_entitlement/time.hpp"

namespace entl {

/// The durable, exact record of what a request produced. Replaying a request
/// that already committed returns the stored outcome verbatim rather than
/// re-evaluating it against current state.
struct MutationOutcome {
  ErrorCode code{ErrorCode::kOk};
  RequestId request_id{};
  Sha256Digest request_digest{};
  Sequence commit_seq{};
  Timestamp committed_at{};
  Revision primary_revision{};
  EntitlementId primary_id{};
  std::vector<EntitlementId> affected{};
  std::string message{};

  /// Set by the store when the outcome came from the idempotency index rather
  /// than from a fresh commit. Never persisted.
  bool replayed{false};

  [[nodiscard]] bool accepted() const noexcept { return code == ErrorCode::kOk; }

  void encode(CanonicalWriter& writer) const;
  static MutationOutcome decode(CanonicalReader& reader);
};

/// Publishes a new authoritative snapshot. Every field is optional: an absent
/// field keeps its current value, so a publication can advance exactly one
/// generation.
struct AuthorityUpdateRequest {
  RequestId request_id{};
  Timestamp now{};
  Revision expected_snapshot_revision{};
  bool advance_epoch{false};
  std::optional<Generation> facility_capacity_generation{};
  std::optional<Revision> facility_policy_revision{};
  std::optional<Revision> resource_envelope_revision{};
  std::optional<Revision> service_class_revision{};
  std::optional<Sha256Digest> policy_context_digest{};
  std::optional<Sha256Digest> envelope_binding_digest{};
  ActorId publisher{};
  std::string note{};
};

struct GrantRequest {
  RequestId request_id{};
  Timestamp now{};
  TenantId holder{};
  ServiceId service{};
  ServiceClassId service_class{};
  ResourceScope scope{};
  Unit unit{Unit::kCount};
  Quantity quantity{};
  Priority priority{};
  Timestamp effective_from{};
  Timestamp expires_at{};
  Sha256Digest admission_decision_digest{};
  std::optional<Sha256Digest> policy_context_digest{};
  FenceMask fence{};
  ActorId actor{};
  std::string note{};
};

struct SuspendRequest {
  RequestId request_id{};
  Timestamp now{};
  EntitlementId id{};
  Revision expected_revision{};
  ActorId actor{};
  std::string note{};
};

struct ResumeRequest {
  RequestId request_id{};
  Timestamp now{};
  EntitlementId id{};
  Revision expected_revision{};
  ActorId actor{};
  std::string note{};
};

struct RevokeRequest {
  RequestId request_id{};
  Timestamp now{};
  EntitlementId id{};
  Revision expected_revision{};
  TerminalReason reason{TerminalReason::kOperatorRevocation};

  /// When true (the default), every live delegated descendant of this record is
  /// revoked in the same commit, because its authority was derived from this
  /// one. Split descendants are independent and are not cascaded.
  bool cascade_delegated_children{true};
  ActorId actor{};
  std::string note{};
};

struct ExpireRequest {
  RequestId request_id{};
  Timestamp now{};
  EntitlementId id{};
  Revision expected_revision{};
  ActorId actor{};
  std::string note{};
};

/// Move, split, or delegate authority out of an existing record.
enum class TransferMode : std::uint8_t {
  /// The same record changes holder. The whole remaining quantity moves; the
  /// entitlement identity, revision, and binding are preserved and no new
  /// authority is minted.
  kMove = 1,

  /// A new independent record is minted for the transferred quantity and the
  /// source permanently loses exactly that quantity. The source keeps the
  /// remainder.
  kSplit = 2,

  /// A new dependent record is minted for the quantity and the source loses
  /// exactly that quantity from its usable remainder, so authority is never
  /// duplicated. Revoking the source cascades to the delegated child.
  kDelegate = 3,
};

[[nodiscard]] bool is_valid_transfer_mode(TransferMode mode) noexcept;
[[nodiscard]] const char* transfer_mode_name(TransferMode mode) noexcept;
[[nodiscard]] std::optional<TransferMode> transfer_mode_from_name(std::string_view name) noexcept;

struct TransferRequest {
  RequestId request_id{};
  Timestamp now{};
  EntitlementId source{};
  Revision expected_revision{};
  TransferMode mode{TransferMode::kMove};
  TenantId target_holder{};

  /// Required for kSplit and kDelegate, forbidden for kMove.
  std::optional<Quantity> quantity{};

  /// Priority for the minted record. It may be equal to, or less important
  /// than, the source priority; it may never be more important.
  Priority priority{};
  ActorId actor{};
  std::string note{};
};

struct MergeRequest {
  RequestId request_id{};
  Timestamp now{};
  EntitlementId first{};
  Revision first_expected_revision{};
  EntitlementId second{};
  Revision second_expected_revision{};
  ActorId actor{};
  std::string note{};
};

struct ReissueRequest {
  RequestId request_id{};
  Timestamp now{};
  EntitlementId id{};
  Revision expected_revision{};

  /// When true (the default) the minted record is bound to the current
  /// authoritative snapshot; the source record is superseded and confers no
  /// further authority either way.
  bool rebind_to_current_authority{true};
  ActorId actor{};
  std::string note{};
};

struct DrawRequest {
  RequestId request_id{};
  Timestamp now{};
  EntitlementId id{};
  Revision expected_revision{};
  Quantity quantity{};
  ActorId actor{};
  std::string note{};
};

struct ReleaseRequest {
  RequestId request_id{};
  Timestamp now{};
  EntitlementId id{};
  Revision expected_revision{};
  Quantity quantity{};
  ActorId actor{};
  std::string note{};
};

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_REQUEST_HPP
