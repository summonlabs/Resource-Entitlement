// Resource Entitlement — authority generations, digests, and fencing.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_BINDING_HPP
#define RESOURCE_ENTITLEMENT_BINDING_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "resource_entitlement/canonical.hpp"
#include "resource_entitlement/error.hpp"
#include "resource_entitlement/strong.hpp"
#include "resource_entitlement/time.hpp"

namespace entl {

/// Which parts of an authority binding are fenced, i.e. must still equal the
/// current authoritative snapshot for the entitlement to confer authority.
enum class FenceBit : std::uint16_t {
  kCapacityGeneration = 1u << 0,
  kPolicyRevision = 1u << 1,
  kEnvelopeRevision = 1u << 2,
  kServiceClassRevision = 1u << 3,
  kControlEpoch = 1u << 4,
  kPolicyContextDigest = 1u << 5,
  kEnvelopeBindingDigest = 1u << 6,
};

inline constexpr std::uint16_t kAllFenceBits = 0x007Fu;

/// A bit set of fence bits. The control-epoch bit is mandatory: an entitlement
/// whose authority cannot be fenced by a new control incarnation would be
/// unsafe, so masks without it are rejected.
class FenceMask {
public:
  constexpr FenceMask() noexcept = default;

  [[nodiscard]] static constexpr FenceMask of(std::uint16_t bits) noexcept { return FenceMask(bits); }

  /// Fences every generation/revision plus the control epoch.
  [[nodiscard]] static constexpr FenceMask standard() noexcept { return FenceMask(0x001Fu); }

  /// Fences every bit, including the evidence digests.
  [[nodiscard]] static constexpr FenceMask all() noexcept { return FenceMask(kAllFenceBits); }

  [[nodiscard]] static constexpr FenceMask control_epoch_only() noexcept {
    return FenceMask(static_cast<std::uint16_t>(FenceBit::kControlEpoch));
  }

  [[nodiscard]] constexpr std::uint16_t bits() const noexcept { return bits_; }
  [[nodiscard]] constexpr bool has(FenceBit bit) const noexcept {
    return (bits_ & static_cast<std::uint16_t>(bit)) != 0u;
  }

  /// True when no unknown bit is set and the control-epoch bit is present.
  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return (bits_ & ~kAllFenceBits) == 0u && has(FenceBit::kControlEpoch);
  }

  friend constexpr bool operator==(const FenceMask&, const FenceMask&) noexcept = default;

  void encode(CanonicalWriter& writer) const { writer.u16(bits_); }
  static FenceMask decode(CanonicalReader& reader) { return FenceMask(reader.u16()); }

private:
  explicit constexpr FenceMask(std::uint16_t bits) noexcept : bits_(bits) {}
  std::uint16_t bits_{0};
};

/// The authoritative generations, revisions, and evidence digests that every
/// new grant is bound to.
///
/// This snapshot is *reported to* this repository by the authorities that own
/// those values (Facility Capacity, Facility Policy Engine, Resource Envelope,
/// Service Class Registry, and the entitlement control plane itself). The
/// repository records and fences against it; it never derives or invents the
/// values.
struct AuthoritySnapshot {
  Revision revision{};
  Generation facility_capacity_generation{};
  Revision facility_policy_revision{};
  Revision resource_envelope_revision{};
  Revision service_class_revision{};
  Epoch control_epoch{};
  Sha256Digest policy_context_digest{};
  Sha256Digest envelope_binding_digest{};
  Timestamp published_at{};
  ActorId publisher{};
  std::string note{};

  /// SHA-256 over the canonical encoding of every field above. A snapshot
  /// digest therefore changes whenever any authoritative input changes.
  [[nodiscard]] Sha256Digest snapshot_digest() const;

  void encode(CanonicalWriter& writer) const;
  static AuthoritySnapshot decode(CanonicalReader& reader);
};

/// The exact authority a single entitlement is bound to, plus the per-grant
/// admission evidence that justified it.
struct AuthorityBinding {
  Revision authority_revision{};
  Generation facility_capacity_generation{};
  Revision facility_policy_revision{};
  Revision resource_envelope_revision{};
  Revision service_class_revision{};
  Epoch control_epoch{};
  Sha256Digest policy_context_digest{};
  Sha256Digest envelope_binding_digest{};
  Sha256Digest admission_decision_digest{};

  [[nodiscard]] Sha256Digest binding_digest() const;

  void encode(CanonicalWriter& writer) const;
  static AuthorityBinding decode(CanonicalReader& reader);
};

/// Deterministic outcome of comparing a binding to the current snapshot under a
/// fence mask. faults is ordered: control epoch, capacity, policy, envelope,
/// service class, policy-context digest, envelope digest.
struct FenceEvaluation {
  bool stale{false};
  ErrorCode primary{ErrorCode::kOk};
  std::vector<ErrorCode> faults{};

  [[nodiscard]] bool epoch_mismatch() const noexcept;
};

[[nodiscard]] FenceEvaluation evaluate_fence(const AuthorityBinding& binding,
                                            const AuthoritySnapshot& snapshot,
                                            const FenceMask& fence);

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_BINDING_HPP
