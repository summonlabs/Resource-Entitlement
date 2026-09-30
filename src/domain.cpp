// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <string>
#include <vector>

#include "resource_entitlement/binding.hpp"
#include "resource_entitlement/decision.hpp"
#include "resource_entitlement/entitlement.hpp"
#include "resource_entitlement/request.hpp"
#include "resource_entitlement/strong.hpp"
#include "resource_entitlement/text.hpp"

namespace entl {
namespace {

constexpr std::size_t kMaxSourcesPerRecord = 8;
constexpr std::size_t kMaxHolderHistory = 1024;
constexpr std::size_t kMaxAffectedPerOutcome = 4096;

void encode_optional_timestamp(CanonicalWriter& writer, const std::optional<Timestamp>& value) {
  writer.presence(value.has_value());
  if (value.has_value()) {
    writer.i64(value->unix_nanos());
  }
}

std::optional<Timestamp> decode_optional_timestamp(CanonicalReader& reader) {
  if (!reader.presence()) {
    return std::nullopt;
  }
  const std::int64_t nanos = reader.i64();
  if (!reader.ok()) {
    return std::nullopt;
  }
  auto parsed = Timestamp::from_unix_nanos(nanos);
  if (!parsed.has_value()) {
    reader.fail();
    return std::nullopt;
  }
  return parsed;
}

std::vector<EntitlementId> decode_id_list(CanonicalReader& reader, std::size_t max_count) {
  const std::uint32_t count = reader.u32();
  if (!reader.ok() || static_cast<std::size_t>(count) > max_count) {
    reader.fail();
    return {};
  }
  std::vector<EntitlementId> out;
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    out.push_back(EntitlementId::decode(reader));
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// AuthoritySnapshot
// ---------------------------------------------------------------------------

void AuthoritySnapshot::encode(CanonicalWriter& writer) const {
  writer.u64(revision.value());
  writer.u64(facility_capacity_generation.value());
  writer.u64(facility_policy_revision.value());
  writer.u64(resource_envelope_revision.value());
  writer.u64(service_class_revision.value());
  writer.u64(control_epoch.value());
  writer.fixed(policy_context_digest.span());
  writer.fixed(envelope_binding_digest.span());
  writer.i64(published_at.unix_nanos());
  publisher.encode(writer);
  writer.text(note);
}

AuthoritySnapshot AuthoritySnapshot::decode(CanonicalReader& reader) {
  AuthoritySnapshot snapshot;
  snapshot.revision = Revision::from_value(reader.u64());
  snapshot.facility_capacity_generation = Generation::from_value(reader.u64());
  snapshot.facility_policy_revision = Revision::from_value(reader.u64());
  snapshot.resource_envelope_revision = Revision::from_value(reader.u64());
  snapshot.service_class_revision = Revision::from_value(reader.u64());
  snapshot.control_epoch = Epoch::from_value(reader.u64());
  const auto policy_digest = reader.fixed(Sha256Digest::kSize);
  const auto envelope_digest = reader.fixed(Sha256Digest::kSize);
  const std::int64_t published = reader.i64();
  snapshot.publisher = ActorId::decode(reader);
  snapshot.note = reader.text(kMaxTextBytes);
  if (!reader.ok()) {
    return snapshot;
  }
  const auto policy = Sha256Digest::from_bytes(policy_digest);
  const auto envelope = Sha256Digest::from_bytes(envelope_digest);
  const auto timestamp = Timestamp::from_unix_nanos(published);
  if (!policy.has_value() || !envelope.has_value() || !timestamp.has_value()) {
    reader.fail();
    return snapshot;
  }
  snapshot.policy_context_digest = policy.value();
  snapshot.envelope_binding_digest = envelope.value();
  snapshot.published_at = timestamp.value();
  return snapshot;
}

Sha256Digest AuthoritySnapshot::snapshot_digest() const {
  CanonicalWriter writer;
  encode(writer);
  return writer.digest();
}

// ---------------------------------------------------------------------------
// AuthorityBinding
// ---------------------------------------------------------------------------

void AuthorityBinding::encode(CanonicalWriter& writer) const {
  writer.u64(authority_revision.value());
  writer.u64(facility_capacity_generation.value());
  writer.u64(facility_policy_revision.value());
  writer.u64(resource_envelope_revision.value());
  writer.u64(service_class_revision.value());
  writer.u64(control_epoch.value());
  writer.fixed(policy_context_digest.span());
  writer.fixed(envelope_binding_digest.span());
  writer.fixed(admission_decision_digest.span());
}

AuthorityBinding AuthorityBinding::decode(CanonicalReader& reader) {
  AuthorityBinding binding;
  binding.authority_revision = Revision::from_value(reader.u64());
  binding.facility_capacity_generation = Generation::from_value(reader.u64());
  binding.facility_policy_revision = Revision::from_value(reader.u64());
  binding.resource_envelope_revision = Revision::from_value(reader.u64());
  binding.service_class_revision = Revision::from_value(reader.u64());
  binding.control_epoch = Epoch::from_value(reader.u64());
  const auto policy = reader.fixed(Sha256Digest::kSize);
  const auto envelope = reader.fixed(Sha256Digest::kSize);
  const auto admission = reader.fixed(Sha256Digest::kSize);
  if (!reader.ok()) {
    return binding;
  }
  const auto policy_digest = Sha256Digest::from_bytes(policy);
  const auto envelope_digest = Sha256Digest::from_bytes(envelope);
  const auto admission_digest = Sha256Digest::from_bytes(admission);
  if (!policy_digest.has_value() || !envelope_digest.has_value() || !admission_digest.has_value()) {
    reader.fail();
    return binding;
  }
  binding.policy_context_digest = policy_digest.value();
  binding.envelope_binding_digest = envelope_digest.value();
  binding.admission_decision_digest = admission_digest.value();
  return binding;
}

Sha256Digest AuthorityBinding::binding_digest() const {
  CanonicalWriter writer;
  encode(writer);
  return writer.digest();
}

// ---------------------------------------------------------------------------
// Fencing
// ---------------------------------------------------------------------------

bool FenceEvaluation::epoch_mismatch() const noexcept {
  return std::find(faults.begin(), faults.end(), ErrorCode::kStaleEpoch) != faults.end();
}

FenceEvaluation evaluate_fence(const AuthorityBinding& binding, const AuthoritySnapshot& snapshot,
                               const FenceMask& fence) {
  FenceEvaluation result;
  if (fence.has(FenceBit::kControlEpoch) && binding.control_epoch != snapshot.control_epoch) {
    result.faults.push_back(ErrorCode::kStaleEpoch);
  }
  if (fence.has(FenceBit::kCapacityGeneration) &&
      binding.facility_capacity_generation != snapshot.facility_capacity_generation) {
    result.faults.push_back(ErrorCode::kStaleBinding);
  }
  if (fence.has(FenceBit::kPolicyRevision) && binding.facility_policy_revision != snapshot.facility_policy_revision) {
    result.faults.push_back(ErrorCode::kStaleBinding);
  }
  if (fence.has(FenceBit::kEnvelopeRevision) &&
      binding.resource_envelope_revision != snapshot.resource_envelope_revision) {
    result.faults.push_back(ErrorCode::kStaleBinding);
  }
  if (fence.has(FenceBit::kServiceClassRevision) &&
      binding.service_class_revision != snapshot.service_class_revision) {
    result.faults.push_back(ErrorCode::kStaleBinding);
  }
  if (fence.has(FenceBit::kPolicyContextDigest) && binding.policy_context_digest != snapshot.policy_context_digest) {
    result.faults.push_back(ErrorCode::kStaleBinding);
  }
  if (fence.has(FenceBit::kEnvelopeBindingDigest) &&
      binding.envelope_binding_digest != snapshot.envelope_binding_digest) {
    result.faults.push_back(ErrorCode::kStaleBinding);
  }
  result.stale = !result.faults.empty();
  result.primary = result.stale ? result.faults.front() : ErrorCode::kOk;
  return result;
}

// ---------------------------------------------------------------------------
// HolderChange
// ---------------------------------------------------------------------------

void HolderChange::encode(CanonicalWriter& writer) const {
  from.encode(writer);
  to.encode(writer);
  writer.i64(at.unix_nanos());
  writer.u64(commit_seq.value());
  actor.encode(writer);
  writer.text(note);
}

HolderChange HolderChange::decode(CanonicalReader& reader) {
  HolderChange change;
  change.from = TenantId::decode(reader);
  change.to = TenantId::decode(reader);
  const std::int64_t at = reader.i64();
  change.commit_seq = Sequence::from_value(reader.u64());
  change.actor = ActorId::decode(reader);
  change.note = reader.text(kMaxTextBytes);
  if (!reader.ok()) {
    return change;
  }
  const auto parsed = Timestamp::from_unix_nanos(at);
  if (!parsed.has_value()) {
    reader.fail();
    return change;
  }
  change.at = parsed.value();
  return change;
}

// ---------------------------------------------------------------------------
// Entitlement
// ---------------------------------------------------------------------------

void Entitlement::encode(CanonicalWriter& writer) const {
  id.encode(writer);
  root.encode(writer);
  writer.u8(static_cast<std::uint8_t>(relation));
  writer.u32(static_cast<std::uint32_t>(sources.size()));
  for (const EntitlementId& source : sources) {
    source.encode(writer);
  }
  writer.u64(revision.value());
  writer.u64(mint_ordinal.value());
  writer.u8(static_cast<std::uint8_t>(state));
  holder.encode(writer);
  origin_holder.encode(writer);
  service.encode(writer);
  service_class.encode(writer);
  scope.encode(writer);
  writer.u8(static_cast<std::uint8_t>(unit));
  granted.encode(writer);
  remaining.encode(writer);
  delegated_out.encode(writer);
  priority.encode(writer);
  writer.i64(effective_from.unix_nanos());
  writer.i64(expires_at.unix_nanos());
  writer.i64(created_at.unix_nanos());
  writer.i64(updated_at.unix_nanos());
  created_by.encode(writer);
  binding.encode(writer);
  fence.encode(writer);
  writer.fixed(request_digest.span());
  writer.text(note);
  encode_optional_timestamp(writer, suspended_at);
  encode_optional_timestamp(writer, revoked_at);
  writer.u8(static_cast<std::uint8_t>(terminal_reason));
  writer.u32(static_cast<std::uint32_t>(holder_history.size()));
  for (const HolderChange& change : holder_history) {
    change.encode(writer);
  }
  writer.u64(last_commit_seq.value());
}

Entitlement Entitlement::decode(CanonicalReader& reader) {
  Entitlement record;
  record.id = EntitlementId::decode(reader);
  record.root = EntitlementId::decode(reader);
  const std::uint8_t relation = reader.u8();
  record.sources = decode_id_list(reader, kMaxSourcesPerRecord);
  record.revision = Revision::from_value(reader.u64());
  record.mint_ordinal = MintOrdinal::from_value(reader.u64());
  const std::uint8_t state = reader.u8();
  record.holder = TenantId::decode(reader);
  record.origin_holder = TenantId::decode(reader);
  record.service = ServiceId::decode(reader);
  record.service_class = ServiceClassId::decode(reader);
  record.scope = ResourceScope::decode(reader);
  const std::uint8_t unit = reader.u8();
  record.granted = Quantity::decode(reader);
  record.remaining = Quantity::decode(reader);
  record.delegated_out = Quantity::decode(reader);
  record.priority = Priority::decode(reader);
  const std::int64_t effective_from = reader.i64();
  const std::int64_t expires_at = reader.i64();
  const std::int64_t created_at = reader.i64();
  const std::int64_t updated_at = reader.i64();
  record.created_by = ActorId::decode(reader);
  record.binding = AuthorityBinding::decode(reader);
  record.fence = FenceMask::decode(reader);
  const auto request_digest = reader.fixed(Sha256Digest::kSize);
  record.note = reader.text(kMaxTextBytes);
  record.suspended_at = decode_optional_timestamp(reader);
  record.revoked_at = decode_optional_timestamp(reader);
  const std::uint8_t terminal_reason = reader.u8();
  const std::uint32_t history_count = reader.u32();
  if (!reader.ok() || static_cast<std::size_t>(history_count) > kMaxHolderHistory) {
    reader.fail();
    return record;
  }
  record.holder_history.reserve(history_count);
  for (std::uint32_t i = 0; i < history_count; ++i) {
    record.holder_history.push_back(HolderChange::decode(reader));
  }
  record.last_commit_seq = Sequence::from_value(reader.u64());
  if (!reader.ok()) {
    return record;
  }

  const auto digest = Sha256Digest::from_bytes(request_digest);
  const auto effective = Timestamp::from_unix_nanos(effective_from);
  const auto expires = Timestamp::from_unix_nanos(expires_at);
  const auto created = Timestamp::from_unix_nanos(created_at);
  const auto updated = Timestamp::from_unix_nanos(updated_at);
  if (!digest.has_value() || !effective.has_value() || !expires.has_value() || !created.has_value() ||
      !updated.has_value()) {
    reader.fail();
    return record;
  }
  record.request_digest = digest.value();
  record.effective_from = effective.value();
  record.expires_at = expires.value();
  record.created_at = created.value();
  record.updated_at = updated.value();

  const auto parsed_unit = static_cast<Unit>(unit);
  const auto parsed_state = static_cast<EntitlementState>(state);
  const auto parsed_relation = static_cast<LineageRelation>(relation);
  const auto parsed_terminal = static_cast<TerminalReason>(terminal_reason);
  const bool valid = is_valid_unit(parsed_unit) && is_valid_entitlement_state(parsed_state) &&
                     is_valid_lineage_relation(parsed_relation) && is_valid_terminal_reason(parsed_terminal) &&
                     record.fence.is_valid() && record.id.is_set() && record.root.is_set() &&
                     record.holder.is_set() && record.origin_holder.is_set() && record.service.is_set() &&
                     record.service_class.is_set() && record.scope.is_set() && record.granted.unit() == parsed_unit &&
                     record.remaining.unit() == parsed_unit && record.delegated_out.unit() == parsed_unit &&
                     record.expires_at > record.effective_from && record.remaining <= record.granted &&
                     record.created_at <= record.updated_at;
  if (!valid) {
    reader.fail();
    return record;
  }
  record.unit = parsed_unit;
  record.state = parsed_state;
  record.relation = parsed_relation;
  record.terminal_reason = parsed_terminal;
  if (record.relation == LineageRelation::kRoot) {
    if (!record.sources.empty() || record.root != record.id) {
      reader.fail();
    }
  } else if (record.sources.empty()) {
    reader.fail();
  }
  return record;
}

// ---------------------------------------------------------------------------
// Liveness
// ---------------------------------------------------------------------------

LivenessView evaluate_liveness(const Entitlement& entitlement, const AuthoritySnapshot& snapshot,
                               const Timestamp& now) {
  LivenessView view;
  const FenceEvaluation fence = evaluate_fence(entitlement.binding, snapshot, entitlement.fence);
  for (const ErrorCode fault : fence.faults) {
    view.faults.push_back(fault);
  }
  view.stale = fence.stale;

  if (entitlement.state == EntitlementState::kRevoked) {
    view.faults.push_back(ErrorCode::kRevoked);
  } else if (entitlement.state == EntitlementState::kSuperseded) {
    view.faults.push_back(ErrorCode::kSuperseded);
  } else if (entitlement.state == EntitlementState::kSuspended) {
    view.faults.push_back(ErrorCode::kSuspended);
  } else if (entitlement.state == EntitlementState::kExpired) {
    view.faults.push_back(ErrorCode::kExpired);
  }

  if (now < entitlement.effective_from) {
    view.faults.push_back(ErrorCode::kNotYetEffective);
  } else if (!(now < entitlement.expires_at)) {
    view.faults.push_back(ErrorCode::kExpired);
  }

  if (entitlement.remaining.is_zero()) {
    view.faults.push_back(ErrorCode::kQuantityExceeded);
  }

  if (view.faults.empty()) {
    view.live = true;
    view.primary = ErrorCode::kOk;
    view.explanation = "entitlement is live authority for its scope at the decision instant";
    return view;
  }

  view.live = false;
  view.primary = view.faults.front();
  std::string explanation = "refused: ";
  explanation.append(error_code_name(view.primary));
  if (view.faults.size() > 1u) {
    explanation.append(" (also ");
    for (std::size_t i = 1; i < view.faults.size(); ++i) {
      if (i > 1u) {
        explanation.append(", ");
      }
      explanation.append(error_code_name(view.faults[i]));
    }
    explanation.append(")");
  }
  view.explanation = explanation;
  return view;
}

// ---------------------------------------------------------------------------
// ExpectedContext / VerificationDecision
// ---------------------------------------------------------------------------

void ExpectedContext::encode(CanonicalWriter& writer) const {
  holder.encode(writer);
  service.encode(writer);
  service_class.encode(writer);
  scope.encode(writer);
  writer.u8(static_cast<std::uint8_t>(unit));
}

ExpectedContext ExpectedContext::decode(CanonicalReader& reader) {
  ExpectedContext context;
  context.holder = TenantId::decode(reader);
  context.service = ServiceId::decode(reader);
  context.service_class = ServiceClassId::decode(reader);
  context.scope = ResourceScope::decode(reader);
  const std::uint8_t unit = reader.u8();
  if (!reader.ok()) {
    return context;
  }
  const auto parsed = static_cast<Unit>(unit);
  if (!is_valid_unit(parsed)) {
    reader.fail();
    return context;
  }
  context.unit = parsed;
  return context;
}

std::string VerificationDecision::to_string() const {
  std::string out;
  out.append(authorized ? "authorized" : "refused");
  out.append(" code=");
  out.append(error_code_name(primary));
  out.append(" id=");
  out.append(id.to_hex());
  out.append(" revision=");
  out.append(std::to_string(revision.value()));
  out.append(" observed_commit_seq=");
  out.append(std::to_string(observed_commit_seq.value()));
  out.append(" at=");
  out.append(decision_time.to_rfc3339());
  if (requested.has_value()) {
    out.append(" requested=");
    out.append(requested->to_string());
  }
  out.append(" remaining=");
  out.append(remaining.to_string());
  if (stale) {
    out.append(" stale=true");
  }
  if (!secondary_faults.empty()) {
    out.append(" secondary=");
    for (std::size_t i = 0; i < secondary_faults.size(); ++i) {
      if (i > 0u) {
        out.append(",");
      }
      out.append(error_code_name(secondary_faults[i]));
    }
  }
  if (!explanation.empty()) {
    out.append(" explanation=");
    out.append(explanation);
  }
  return out;
}

// ---------------------------------------------------------------------------
// MutationOutcome
// ---------------------------------------------------------------------------

void MutationOutcome::encode(CanonicalWriter& writer) const {
  writer.u16(static_cast<std::uint16_t>(code));
  request_id.encode(writer);
  writer.fixed(request_digest.span());
  writer.u64(commit_seq.value());
  writer.i64(committed_at.unix_nanos());
  writer.u64(primary_revision.value());
  primary_id.encode(writer);
  writer.u32(static_cast<std::uint32_t>(affected.size()));
  for (const EntitlementId& entry : affected) {
    entry.encode(writer);
  }
  writer.text(message);
}

MutationOutcome MutationOutcome::decode(CanonicalReader& reader) {
  MutationOutcome outcome;
  const std::uint16_t code = reader.u16();
  outcome.request_id = RequestId::decode(reader);
  const auto request_digest = reader.fixed(Sha256Digest::kSize);
  outcome.commit_seq = Sequence::from_value(reader.u64());
  const std::int64_t committed_at = reader.i64();
  outcome.primary_revision = Revision::from_value(reader.u64());
  outcome.primary_id = EntitlementId::decode(reader);
  outcome.affected = decode_id_list(reader, kMaxAffectedPerOutcome);
  outcome.message = reader.text(kMaxTextBytes);
  if (!reader.ok()) {
    return outcome;
  }
  const auto digest = Sha256Digest::from_bytes(request_digest);
  const auto timestamp = Timestamp::from_unix_nanos(committed_at);
  if (!digest.has_value() || !timestamp.has_value()) {
    reader.fail();
    return outcome;
  }
  outcome.code = static_cast<ErrorCode>(code);
  if (code > static_cast<std::uint16_t>(ErrorCode::kInternalError)) {
    reader.fail();
    return outcome;
  }
  outcome.request_digest = digest.value();
  outcome.committed_at = timestamp.value();
  outcome.replayed = false;
  return outcome;
}

}  // namespace entl
