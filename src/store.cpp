// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "resource_entitlement/store.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "durable.hpp"
#include "fileio.hpp"
#include "resource_entitlement/decision.hpp"
#include "resource_entitlement/text.hpp"
#include "resource_entitlement/token.hpp"

namespace entl {
namespace {

constexpr std::size_t kDefaultListLimit = 1024;
constexpr std::size_t kMaxListLimit = 65536;

[[nodiscard]] Error make_error(ErrorCode code, std::string message, std::string detail = {}) {
  return Error(code, std::move(message), std::move(detail));
}

[[nodiscard]] Result<Sequence> next_sequence(const Sequence& current) {
  auto next = current.next();
  if (!next.has_value()) {
    return make_error(ErrorCode::kCapacityExhausted, "commit sequence is exhausted");
  }
  return next.value();
}

[[nodiscard]] Result<Revision> next_revision(const Revision& current) {
  auto next = current.next();
  if (!next.has_value()) {
    return make_error(ErrorCode::kCapacityExhausted, "record revision is exhausted");
  }
  return next.value();
}

[[nodiscard]] Result<Epoch> next_epoch(const Epoch& current) {
  auto next = current.next();
  if (!next.has_value()) {
    return make_error(ErrorCode::kCapacityExhausted, "control epoch is exhausted");
  }
  return next.value();
}

[[nodiscard]] Result<MintOrdinal> next_mint(const MintOrdinal& current) {
  auto next = current.next();
  if (!next.has_value()) {
    return make_error(ErrorCode::kCapacityExhausted, "mint ordinal is exhausted");
  }
  return next.value();
}

void append_fault(std::vector<ErrorCode>& faults, ErrorCode code) {
  if (std::find(faults.begin(), faults.end(), code) == faults.end()) {
    faults.push_back(code);
  }
}

[[nodiscard]] std::string join_faults(const std::vector<ErrorCode>& faults) {
  std::string out;
  for (std::size_t i = 0; i < faults.size(); ++i) {
    if (i > 0u) {
      out.append(", ");
    }
    out.append(error_code_name(faults[i]));
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Store::Impl
// ---------------------------------------------------------------------------

struct Store::Impl {
  StoreOpenOptions options{};
  std::unique_ptr<detail::DurableStore> durable{};
  detail::StateCore state{};
  mutable std::shared_mutex mutex{};
  bool read_only{false};
  StoreStats stats{};
  std::size_t log_segment_count{0};
  std::uint64_t open_log_bytes{0};
  InspectionReport open_report{};
  std::string open_message{};
  std::vector<Event> pending_events{};

  [[nodiscard]] bool lock_held() const noexcept { return !read_only && durable != nullptr; }

  void queue_event(EventKind kind, Sequence seq, Timestamp at, const EntitlementId& id, Revision revision,
                   ErrorCode code, std::string detail) {
    Event event;
    event.kind = kind;
    event.commit_seq = seq;
    event.at = at;
    event.id = id;
    event.revision = revision;
    event.code = code;
    event.detail = std::move(detail);
    pending_events.push_back(std::move(event));
  }

  void emit(const std::vector<Event>& events) {
    if (!options.event_sink) {
      return;
    }
    for (const Event& event : events) {
      options.event_sink(event);
    }
  }

  /// Persisted free text must satisfy the strict reader's text rules.
  [[nodiscard]] Result<void> validate_persistable_text(const detail::JournalEntry& entry) const {
    if (!is_valid_text(entry.outcome.message)) {
      return make_error(ErrorCode::kInvalidText,
                        "the outcome message is not acceptable persisted text");
    }
    if (!is_valid_text(entry.authority_after.note)) {
      return make_error(ErrorCode::kInvalidText, "the authority note is not acceptable persisted text");
    }
    for (const Entitlement& record : entry.post_images) {
      if (!is_valid_text(record.note)) {
        return make_error(ErrorCode::kInvalidText,
                          "an entitlement note is not acceptable persisted text",
                          record.note.size() > kMaxTextBytes ? "note exceeds the text bound"
                                                             : "note contains a rejected character");
      }
      for (const HolderChange& change : record.holder_history) {
        if (!is_valid_text(change.note)) {
          return make_error(ErrorCode::kInvalidText,
                            "a holder-change note is not acceptable persisted text");
        }
      }
    }
    return {};
  }

  /// Single choke point through which every durable record passes.
  ///
  /// A mutation must never be able to persist a record that the store's own
  /// strict reader would later reject: doing so would turn a caller mistake
  /// into an unreadable ledger. Every post-image and the authority snapshot are
  /// therefore re-encoded, re-decoded, and re-encoded again, and must round trip
  /// byte for byte, and all persisted free text must satisfy the reader's rules.
  [[nodiscard]] Result<void> validate_persistable(const detail::JournalEntry& entry) const {
    auto text_ok = validate_persistable_text(entry);
    if (!text_ok) {
      return text_ok.error();
    }
    for (const Entitlement& record : entry.post_images) {
      if (record.created_at > record.updated_at) {
        return make_error(ErrorCode::kInvalidTimestamp,
                          "the decision instant precedes the record's creation instant",
                          record.id.to_hex());
      }
      CanonicalWriter writer;
      record.encode(writer);
      CanonicalReader reader(writer.data());
      Entitlement decoded = Entitlement::decode(reader);
      auto finished = reader.finish();
      if (!finished.has_value()) {
        return make_error(ErrorCode::kInvalidArgument,
                          "the mutation would persist a record the store's reader rejects",
                          std::string(finished.error().message()) + " for " + record.id.to_hex());
      }
      CanonicalWriter again;
      decoded.encode(again);
      if (again.data() != writer.data()) {
        return make_error(ErrorCode::kInternalError,
                          "a persisted record does not round trip byte for byte", record.id.to_hex());
      }
    }
    {
      CanonicalWriter writer;
      entry.authority_after.encode(writer);
      CanonicalReader reader(writer.data());
      AuthoritySnapshot decoded = AuthoritySnapshot::decode(reader);
      auto finished = reader.finish();
      if (!finished.has_value()) {
        return make_error(ErrorCode::kInvalidArgument,
                          "the mutation would persist an authority snapshot the store's reader rejects",
                          finished.error().message());
      }
      CanonicalWriter again;
      decoded.encode(again);
      if (again.data() != writer.data()) {
        return make_error(ErrorCode::kInternalError,
                          "the authority snapshot does not round trip byte for byte");
      }
    }
    return {};
  }

  [[nodiscard]] Result<MutationOutcome> commit_entry(std::uint8_t kind, MutationOutcome outcome,
                                                     detail::StateCore&& next_state,
                                                     std::vector<Entitlement> post_images,
                                                     bool authority_changed, EventKind event_kind,
                                                     std::string event_detail) {
    outcome.commit_seq = next_state.commit_seq;
    detail::record_idempotency(next_state, outcome);
    detail::JournalEntry entry;
    entry.kind = kind;
    entry.outcome = outcome;
    entry.post_images = std::move(post_images);
    entry.authority_after = next_state.authority;
    entry.authority_changed = authority_changed;
    auto persistable = validate_persistable(entry);
    if (!persistable) {
      return persistable.error();
    }
    auto committed = durable->commit(entry, next_state);
    if (!committed) {
      return committed.error();
    }
    state = std::move(next_state);
    stats.commits += 1u;
    queue_event(event_kind, outcome.commit_seq, outcome.committed_at, outcome.primary_id,
                outcome.primary_revision, ErrorCode::kOk, std::move(event_detail));
    return outcome;
  }

  [[nodiscard]] std::optional<MutationOutcome> find_idempotent(const RequestId& request_id,
                                                               const Sha256Digest& request_digest,
                                                               Error* conflict) {
    for (auto it = state.idempotency.rbegin(); it != state.idempotency.rend(); ++it) {
      if (it->request_id != request_id) {
        continue;
      }
      if (it->outcome.request_digest != request_digest) {
        if (conflict != nullptr) {
          *conflict = make_error(ErrorCode::kRequestIdConflict,
                                 "this request identity was already used with a different payload");
        }
        return std::nullopt;
      }
      MutationOutcome replay = it->outcome;
      replay.replayed = true;
      stats.replays += 1u;
      return replay;
    }
    return std::nullopt;
  }

  [[nodiscard]] MutationOutcome make_outcome(const RequestId& request_id, const Sha256Digest& request_digest,
                                             Timestamp now, std::string message) const {
    MutationOutcome outcome;
    outcome.code = ErrorCode::kOk;
    outcome.request_id = request_id;
    outcome.request_digest = request_digest;
    outcome.committed_at = now;
    outcome.message = std::move(message);
    return outcome;
  }

  [[nodiscard]] Result<EntitlementId> mint_identity(const RequestId& request_id,
                                                    const MintOrdinal& ordinal) {
    CanonicalWriter writer;
    writer.fixed(request_id.span());
    writer.u64(ordinal.value());
    const EntitlementId id = EntitlementId::derive(writer.span());
    if (!id.is_set()) {
      return make_error(ErrorCode::kInternalError, "derived identity is degenerate");
    }
    if (state.find(id) != nullptr) {
      return make_error(ErrorCode::kDuplicateIdentity, "derived identity already exists in the ledger");
    }
    return id;
  }

  // --- validation helpers -------------------------------------------------

  [[nodiscard]] Result<void> require_instant(const Timestamp& now) const {
    if (now.is_zero()) {
      return make_error(ErrorCode::kInvalidTimestamp,
                        "the zero instant means 'not supplied'; supply an explicit decision instant");
    }
    return {};
  }

  [[nodiscard]] Result<void> require_actor(const ActorId& actor, const char* field) const {
    if (!actor.is_set()) {
      return make_error(ErrorCode::kMissingField, std::string(field) + " must be supplied");
    }
    return {};
  }

  [[nodiscard]] Result<const Entitlement*> require_record(const EntitlementId& id,
                                                          const Revision& expected) const {
    const Entitlement* record = state.find(id);
    if (record == nullptr) {
      return make_error(ErrorCode::kUnknownEntitlement, "no entitlement with that identity exists",
                        id.to_hex());
    }
    if (record->revision != expected) {
      return make_error(ErrorCode::kRevisionConflict, "expected revision does not match the current revision",
                        "expected=" + std::to_string(expected.value()) +
                            " actual=" + std::to_string(record->revision.value()));
    }
    return record;
  }

  [[nodiscard]] Result<void> require_live(const Entitlement& record, const Timestamp& now) const {
    const LivenessView view = evaluate_liveness(record, state.authority, now);
    if (!view.live) {
      return make_error(view.primary, "the entitlement does not confer live authority at this instant",
                        view.explanation);
    }
    return {};
  }

  // --- authority ----------------------------------------------------------

  [[nodiscard]] Result<MutationOutcome> apply_authority_update(const AuthorityUpdateRequest& request) {
    CanonicalWriter digest_writer;
    digest_writer.u8(static_cast<std::uint8_t>(detail::JournalKind::kAuthorityUpdate));
    digest_writer.fixed(request.request_id.span());
    digest_writer.i64(request.now.unix_nanos());
    digest_writer.u64(request.expected_snapshot_revision.value());
    digest_writer.boolean(request.advance_epoch);
    digest_writer.presence(request.facility_capacity_generation.has_value());
    if (request.facility_capacity_generation.has_value()) {
      digest_writer.u64(request.facility_capacity_generation->value());
    }
    digest_writer.presence(request.facility_policy_revision.has_value());
    if (request.facility_policy_revision.has_value()) {
      digest_writer.u64(request.facility_policy_revision->value());
    }
    digest_writer.presence(request.resource_envelope_revision.has_value());
    if (request.resource_envelope_revision.has_value()) {
      digest_writer.u64(request.resource_envelope_revision->value());
    }
    digest_writer.presence(request.service_class_revision.has_value());
    if (request.service_class_revision.has_value()) {
      digest_writer.u64(request.service_class_revision->value());
    }
    digest_writer.presence(request.policy_context_digest.has_value());
    if (request.policy_context_digest.has_value()) {
      digest_writer.fixed(request.policy_context_digest->span());
    }
    digest_writer.presence(request.envelope_binding_digest.has_value());
    if (request.envelope_binding_digest.has_value()) {
      digest_writer.fixed(request.envelope_binding_digest->span());
    }
    request.publisher.encode(digest_writer);
    digest_writer.text(request.note);
    const Sha256Digest request_digest = digest_writer.digest();

    auto validated = require_instant(request.now);
    if (!validated) {
      return validated.error();
    }
    auto actor = require_actor(request.publisher, "publisher");
    if (!actor) {
      return actor.error();
    }
    Error conflict{};
    if (auto replay = find_idempotent(request.request_id, request_digest, &conflict)) {
      return replay.value();
    }
    if (!conflict.message().empty()) {
      return conflict;
    }
    if (state.authority.revision != request.expected_snapshot_revision) {
      return make_error(ErrorCode::kRevisionConflict,
                        "expected authority snapshot revision does not match the current revision",
                        "expected=" + std::to_string(request.expected_snapshot_revision.value()) +
                            " actual=" + std::to_string(state.authority.revision.value()));
    }

    detail::StateCore next = state;
    auto revision = next_revision(next.authority.revision);
    if (!revision) {
      return revision.error();
    }
    next.authority.revision = revision.value();
    if (request.facility_capacity_generation.has_value()) {
      next.authority.facility_capacity_generation = request.facility_capacity_generation.value();
    }
    if (request.facility_policy_revision.has_value()) {
      next.authority.facility_policy_revision = request.facility_policy_revision.value();
    }
    if (request.resource_envelope_revision.has_value()) {
      next.authority.resource_envelope_revision = request.resource_envelope_revision.value();
    }
    if (request.service_class_revision.has_value()) {
      next.authority.service_class_revision = request.service_class_revision.value();
    }
    if (request.policy_context_digest.has_value()) {
      next.authority.policy_context_digest = request.policy_context_digest.value();
    }
    if (request.envelope_binding_digest.has_value()) {
      next.authority.envelope_binding_digest = request.envelope_binding_digest.value();
    }
    if (request.advance_epoch) {
      auto epoch = next_epoch(next.authority.control_epoch);
      if (!epoch) {
        return epoch.error();
      }
      next.authority.control_epoch = epoch.value();
    }
    next.authority.published_at = request.now;
    next.authority.publisher = request.publisher;
    next.authority.note = request.note;
    auto sequence = next_sequence(next.commit_seq);
    if (!sequence) {
      return sequence.error();
    }
    next.commit_seq = sequence.value();

    MutationOutcome outcome =
        make_outcome(request.request_id, request_digest, request.now,
                     "authority snapshot published at revision " +
                         std::to_string(next.authority.revision.value()) + " and control epoch " +
                         std::to_string(next.authority.control_epoch.value()));
    outcome.primary_revision = next.authority.revision;
    return commit_entry(static_cast<std::uint8_t>(detail::JournalKind::kAuthorityUpdate), outcome,
                        std::move(next), {}, true, EventKind::kAuthorityPublished,
                        "revision=" + std::to_string(state.authority.revision.value()) +
                            " epoch=" + std::to_string(state.authority.control_epoch.value()));
  }

  // --- grant --------------------------------------------------------------

  [[nodiscard]] Result<MutationOutcome> apply_grant(const GrantRequest& request) {
    CanonicalWriter digest_writer;
    digest_writer.u8(static_cast<std::uint8_t>(detail::JournalKind::kGrant));
    digest_writer.fixed(request.request_id.span());
    digest_writer.i64(request.now.unix_nanos());
    request.holder.encode(digest_writer);
    request.service.encode(digest_writer);
    request.service_class.encode(digest_writer);
    request.scope.encode(digest_writer);
    digest_writer.u8(static_cast<std::uint8_t>(request.unit));
    digest_writer.u64(request.quantity.units());
    digest_writer.u8(static_cast<std::uint8_t>(request.priority.klass));
    digest_writer.u16(request.priority.rank);
    digest_writer.i64(request.effective_from.unix_nanos());
    digest_writer.i64(request.expires_at.unix_nanos());
    digest_writer.fixed(request.admission_decision_digest.span());
    digest_writer.presence(request.policy_context_digest.has_value());
    if (request.policy_context_digest.has_value()) {
      digest_writer.fixed(request.policy_context_digest->span());
    }
    digest_writer.u16(request.fence.bits());
    request.actor.encode(digest_writer);
    digest_writer.text(request.note);
    const Sha256Digest request_digest = digest_writer.digest();

    auto validated = require_instant(request.now);
    if (!validated) {
      return validated.error();
    }
    if (!request.holder.is_set()) {
      return make_error(ErrorCode::kMissingField, "grant requires a holder tenant");
    }
    if (!request.service.is_set()) {
      return make_error(ErrorCode::kMissingField, "grant requires a service identity");
    }
    if (!request.service_class.is_set()) {
      return make_error(ErrorCode::kMissingField, "grant requires a service class identity");
    }
    if (!request.scope.is_set()) {
      return make_error(ErrorCode::kMissingField, "grant requires a complete resource scope");
    }
    if (!is_valid_unit(request.unit)) {
      return make_error(ErrorCode::kInvalidUnit, "grant unit is not a known unit");
    }
    if (request.quantity.unit() != request.unit) {
      return make_error(ErrorCode::kUnitMismatch, "grant quantity unit disagrees with the declared unit");
    }
    if (request.quantity.is_zero()) {
      return make_error(ErrorCode::kInvalidQuantity, "grant quantity must be greater than zero");
    }
    if (!is_valid_priority_class(request.priority.klass)) {
      return make_error(ErrorCode::kInvalidPriority, "grant priority class is not valid");
    }
    if (request.effective_from.is_zero()) {
      return make_error(ErrorCode::kInvalidTimestamp,
                        "grant effective_from must be an explicit non-zero instant");
    }
    if (!(request.effective_from < request.expires_at)) {
      return make_error(ErrorCode::kWindowViolation, "grant expiry must be strictly after its effective time",
                        "effective_from=" + request.effective_from.to_rfc3339() +
                            " expires_at=" + request.expires_at.to_rfc3339());
    }
    if (!request.fence.is_valid()) {
      return make_error(ErrorCode::kInvalidFenceMask,
                        "fence mask must be a known bit set and must include the control-epoch bit");
    }
    if (options.require_admission_evidence && request.admission_decision_digest.is_zero()) {
      return make_error(ErrorCode::kMissingAdmissionEvidence,
                        "grant requires the admission decision digest that justifies it");
    }
    auto actor = require_actor(request.actor, "actor");
    if (!actor) {
      return actor.error();
    }
    Error conflict{};
    if (auto replay = find_idempotent(request.request_id, request_digest, &conflict)) {
      return replay.value();
    }
    if (!conflict.message().empty()) {
      return conflict;
    }

    detail::StateCore next = state;
    auto ordinal = next_mint(next.mint_ordinal);
    if (!ordinal) {
      return ordinal.error();
    }
    auto identifier = mint_identity(request.request_id, ordinal.value());
    if (!identifier) {
      return identifier.error();
    }
    auto sequence = next_sequence(next.commit_seq);
    if (!sequence) {
      return sequence.error();
    }
    next.commit_seq = sequence.value();
    next.mint_ordinal = ordinal.value();

    Entitlement record;
    record.id = identifier.value();
    record.root = record.id;
    record.relation = LineageRelation::kRoot;
    record.sources.clear();
    record.revision = Revision::from_value(1u);
    record.mint_ordinal = ordinal.value();
    record.state = EntitlementState::kActive;
    record.holder = request.holder;
    record.origin_holder = request.holder;
    record.service = request.service;
    record.service_class = request.service_class;
    record.scope = request.scope;
    record.unit = request.unit;
    record.granted = request.quantity;
    record.remaining = request.quantity;
    record.delegated_out = Quantity::make(request.unit, 0u).value();
    record.priority = request.priority;
    record.effective_from = request.effective_from;
    record.expires_at = request.expires_at;
    record.created_at = request.now;
    record.updated_at = request.now;
    record.created_by = request.actor;
    record.fence = request.fence;
    record.request_digest = request_digest;
    record.note = request.note;
    record.terminal_reason = TerminalReason::kNotApplicable;
    record.last_commit_seq = next.commit_seq;
    record.binding.authority_revision = next.authority.revision;
    record.binding.facility_capacity_generation = next.authority.facility_capacity_generation;
    record.binding.facility_policy_revision = next.authority.facility_policy_revision;
    record.binding.resource_envelope_revision = next.authority.resource_envelope_revision;
    record.binding.service_class_revision = next.authority.service_class_revision;
    record.binding.control_epoch = next.authority.control_epoch;
    record.binding.policy_context_digest = request.policy_context_digest.value_or(
        next.authority.policy_context_digest);
    record.binding.envelope_binding_digest = next.authority.envelope_binding_digest;
    record.binding.admission_decision_digest = request.admission_decision_digest;

    next.insert_or_replace(record);

    MutationOutcome outcome = make_outcome(request.request_id, request_digest, request.now,
                                           "entitlement granted and bound to authority revision " +
                                               std::to_string(next.authority.revision.value()));
    outcome.primary_id = record.id;
    outcome.primary_revision = record.revision;
    outcome.affected = {record.id};
    return commit_entry(static_cast<std::uint8_t>(detail::JournalKind::kGrant), outcome, std::move(next),
                        {record}, false, EventKind::kGranted,
                        record.id.to_hex() + " granted " + record.granted.to_string());
  }

  // --- simple lifecycle ---------------------------------------------------

  [[nodiscard]] Result<MutationOutcome> apply_suspend(const SuspendRequest& request) {
    CanonicalWriter digest_writer;
    digest_writer.u8(static_cast<std::uint8_t>(detail::JournalKind::kSuspend));
    digest_writer.fixed(request.request_id.span());
    digest_writer.i64(request.now.unix_nanos());
    digest_writer.fixed(request.id.span());
    digest_writer.u64(request.expected_revision.value());
    request.actor.encode(digest_writer);
    digest_writer.text(request.note);
    const Sha256Digest request_digest = digest_writer.digest();

    auto validated = require_instant(request.now);
    if (!validated) {
      return validated.error();
    }
    auto actor = require_actor(request.actor, "actor");
    if (!actor) {
      return actor.error();
    }
    Error conflict{};
    if (auto replay = find_idempotent(request.request_id, request_digest, &conflict)) {
      return replay.value();
    }
    if (!conflict.message().empty()) {
      return conflict;
    }
    auto found = require_record(request.id, request.expected_revision);
    if (!found) {
      return found.error();
    }
    const Entitlement& current = *found.value();
    if (current.state != EntitlementState::kActive) {
      return make_error(ErrorCode::kInvalidTransition, "only an active entitlement can be suspended",
                        entitlement_state_name(current.state));
    }
    if (!(request.now < current.expires_at)) {
      return make_error(ErrorCode::kExpired, "the entitlement is already past its expiry instant");
    }

    detail::StateCore next = state;
    Entitlement updated = current;
    auto revision = next_revision(updated.revision);
    if (!revision) {
      return revision.error();
    }
    auto sequence = next_sequence(next.commit_seq);
    if (!sequence) {
      return sequence.error();
    }
    next.commit_seq = sequence.value();
    updated.revision = revision.value();
    updated.state = EntitlementState::kSuspended;
    updated.suspended_at = request.now;
    updated.updated_at = request.now;
    updated.last_commit_seq = next.commit_seq;
    if (!request.note.empty()) {
      updated.note = request.note;
    }
    next.insert_or_replace(updated);

    MutationOutcome outcome = make_outcome(request.request_id, request_digest, request.now,
                                           "entitlement suspended at " + request.now.to_rfc3339());
    outcome.primary_id = updated.id;
    outcome.primary_revision = updated.revision;
    outcome.affected = {updated.id};
    return commit_entry(static_cast<std::uint8_t>(detail::JournalKind::kSuspend), outcome, std::move(next),
                        {updated}, false, EventKind::kSuspended, updated.id.to_hex());
  }

  [[nodiscard]] Result<MutationOutcome> apply_resume(const ResumeRequest& request) {
    CanonicalWriter digest_writer;
    digest_writer.u8(static_cast<std::uint8_t>(detail::JournalKind::kResume));
    digest_writer.fixed(request.request_id.span());
    digest_writer.i64(request.now.unix_nanos());
    digest_writer.fixed(request.id.span());
    digest_writer.u64(request.expected_revision.value());
    request.actor.encode(digest_writer);
    digest_writer.text(request.note);
    const Sha256Digest request_digest = digest_writer.digest();

    auto validated = require_instant(request.now);
    if (!validated) {
      return validated.error();
    }
    auto actor = require_actor(request.actor, "actor");
    if (!actor) {
      return actor.error();
    }
    Error conflict{};
    if (auto replay = find_idempotent(request.request_id, request_digest, &conflict)) {
      return replay.value();
    }
    if (!conflict.message().empty()) {
      return conflict;
    }
    auto found = require_record(request.id, request.expected_revision);
    if (!found) {
      return found.error();
    }
    const Entitlement& current = *found.value();
    if (current.state != EntitlementState::kSuspended) {
      return make_error(ErrorCode::kInvalidTransition, "only a suspended entitlement can be resumed",
                        entitlement_state_name(current.state));
    }
    if (!(request.now < current.expires_at)) {
      return make_error(ErrorCode::kExpired, "the entitlement is already past its expiry instant");
    }

    detail::StateCore next = state;
    Entitlement updated = current;
    auto revision = next_revision(updated.revision);
    if (!revision) {
      return revision.error();
    }
    auto sequence = next_sequence(next.commit_seq);
    if (!sequence) {
      return sequence.error();
    }
    next.commit_seq = sequence.value();
    updated.revision = revision.value();
    updated.state = EntitlementState::kActive;
    updated.suspended_at.reset();
    updated.updated_at = request.now;
    updated.last_commit_seq = next.commit_seq;
    if (!request.note.empty()) {
      updated.note = request.note;
    }
    next.insert_or_replace(updated);

    MutationOutcome outcome = make_outcome(request.request_id, request_digest, request.now,
                                           "entitlement resumed at " + request.now.to_rfc3339());
    outcome.primary_id = updated.id;
    outcome.primary_revision = updated.revision;
    outcome.affected = {updated.id};
    return commit_entry(static_cast<std::uint8_t>(detail::JournalKind::kResume), outcome, std::move(next),
                        {updated}, false, EventKind::kResumed, updated.id.to_hex());
  }

  [[nodiscard]] Result<MutationOutcome> apply_revoke(const RevokeRequest& request) {
    CanonicalWriter digest_writer;
    digest_writer.u8(static_cast<std::uint8_t>(detail::JournalKind::kRevoke));
    digest_writer.fixed(request.request_id.span());
    digest_writer.i64(request.now.unix_nanos());
    digest_writer.fixed(request.id.span());
    digest_writer.u64(request.expected_revision.value());
    digest_writer.u8(static_cast<std::uint8_t>(request.reason));
    digest_writer.boolean(request.cascade_delegated_children);
    request.actor.encode(digest_writer);
    digest_writer.text(request.note);
    const Sha256Digest request_digest = digest_writer.digest();

    auto validated = require_instant(request.now);
    if (!validated) {
      return validated.error();
    }
    auto actor = require_actor(request.actor, "actor");
    if (!actor) {
      return actor.error();
    }
    if (request.reason != TerminalReason::kOperatorRevocation &&
        request.reason != TerminalReason::kPolicyRevocation) {
      return make_error(ErrorCode::kInvalidArgument,
                        "revocation reason must be operator_revocation or policy_revocation");
    }
    Error conflict{};
    if (auto replay = find_idempotent(request.request_id, request_digest, &conflict)) {
      return replay.value();
    }
    if (!conflict.message().empty()) {
      return conflict;
    }
    auto found = require_record(request.id, request.expected_revision);
    if (!found) {
      return found.error();
    }
    const Entitlement& current = *found.value();
    if (is_terminal_state(current.state)) {
      return make_error(ErrorCode::kInvalidTransition, "a terminal entitlement cannot be revoked again",
                        entitlement_state_name(current.state));
    }

    detail::StateCore next = state;
    auto sequence = next_sequence(next.commit_seq);
    if (!sequence) {
      return sequence.error();
    }
    next.commit_seq = sequence.value();

    std::vector<Entitlement> post_images;
    std::vector<EntitlementId> affected;

    Entitlement root_record = current;
    auto revision = next_revision(root_record.revision);
    if (!revision) {
      return revision.error();
    }
    root_record.revision = revision.value();
    root_record.state = EntitlementState::kRevoked;
    root_record.revoked_at = request.now;
    root_record.updated_at = request.now;
    root_record.last_commit_seq = next.commit_seq;
    root_record.terminal_reason = request.reason;
    if (!request.note.empty()) {
      root_record.note = request.note;
    }
    next.insert_or_replace(root_record);
    post_images.push_back(root_record);
    affected.push_back(root_record.id);

    if (request.cascade_delegated_children) {
      std::vector<EntitlementId> frontier{root_record.id};
      for (std::size_t cursor = 0; cursor < frontier.size(); ++cursor) {
        const EntitlementId parent = frontier[cursor];
        for (const Entitlement& candidate : state.entitlements) {
          if (candidate.relation != LineageRelation::kDelegatedFrom) {
            continue;
          }
          if (candidate.sources.size() != 1u || !(candidate.sources.front() == parent)) {
            continue;
          }
          if (is_terminal_state(candidate.state)) {
            continue;
          }
          Entitlement child = candidate;
          auto child_revision = next_revision(child.revision);
          if (!child_revision) {
            return child_revision.error();
          }
          child.revision = child_revision.value();
          child.state = EntitlementState::kRevoked;
          child.revoked_at = request.now;
          child.updated_at = request.now;
          child.last_commit_seq = next.commit_seq;
          child.terminal_reason = TerminalReason::kParentRevoked;
          child.note = request.note.empty() ? std::string("cascaded from parent revocation") : request.note;
          next.insert_or_replace(child);
          post_images.push_back(child);
          affected.push_back(child.id);
          frontier.push_back(child.id);
        }
      }
    }

    MutationOutcome outcome = make_outcome(
        request.request_id, request_digest, request.now,
        "entitlement revoked" + std::string(request.cascade_delegated_children ? " with delegated cascade" : ""));
    outcome.primary_id = root_record.id;
    outcome.primary_revision = root_record.revision;
    outcome.affected = affected;
    return commit_entry(static_cast<std::uint8_t>(detail::JournalKind::kRevoke), outcome, std::move(next),
                        std::move(post_images), false, EventKind::kRevoked,
                        root_record.id.to_hex() + " cascaded=" + std::to_string(affected.size() - 1u));
  }

  [[nodiscard]] Result<MutationOutcome> apply_expire(const ExpireRequest& request) {
    CanonicalWriter digest_writer;
    digest_writer.u8(static_cast<std::uint8_t>(detail::JournalKind::kExpire));
    digest_writer.fixed(request.request_id.span());
    digest_writer.i64(request.now.unix_nanos());
    digest_writer.fixed(request.id.span());
    digest_writer.u64(request.expected_revision.value());
    request.actor.encode(digest_writer);
    digest_writer.text(request.note);
    const Sha256Digest request_digest = digest_writer.digest();

    auto validated = require_instant(request.now);
    if (!validated) {
      return validated.error();
    }
    auto actor = require_actor(request.actor, "actor");
    if (!actor) {
      return actor.error();
    }
    Error conflict{};
    if (auto replay = find_idempotent(request.request_id, request_digest, &conflict)) {
      return replay.value();
    }
    if (!conflict.message().empty()) {
      return conflict;
    }
    auto found = require_record(request.id, request.expected_revision);
    if (!found) {
      return found.error();
    }
    const Entitlement& current = *found.value();
    if (current.state == EntitlementState::kExpired) {
      return make_error(ErrorCode::kInvalidTransition, "the entitlement is already recorded as expired");
    }
    if (is_terminal_state(current.state)) {
      return make_error(ErrorCode::kInvalidTransition, "a terminal entitlement cannot be expired",
                        entitlement_state_name(current.state));
    }
    if (request.now < current.expires_at) {
      return make_error(ErrorCode::kInvalidTransition,
                        "the entitlement has not reached its expiry instant and cannot be expired",
                        "expires_at=" + current.expires_at.to_rfc3339());
    }

    detail::StateCore next = state;
    Entitlement updated = current;
    auto revision = next_revision(updated.revision);
    if (!revision) {
      return revision.error();
    }
    auto sequence = next_sequence(next.commit_seq);
    if (!sequence) {
      return sequence.error();
    }
    next.commit_seq = sequence.value();
    updated.revision = revision.value();
    updated.state = EntitlementState::kExpired;
    updated.terminal_reason = TerminalReason::kExpiredByTime;
    updated.updated_at = request.now;
    updated.last_commit_seq = next.commit_seq;
    next.insert_or_replace(updated);

    MutationOutcome outcome = make_outcome(request.request_id, request_digest, request.now,
                                           "entitlement recorded as expired");
    outcome.primary_id = updated.id;
    outcome.primary_revision = updated.revision;
    outcome.affected = {updated.id};
    return commit_entry(static_cast<std::uint8_t>(detail::JournalKind::kExpire), outcome, std::move(next),
                        {updated}, false, EventKind::kExpired, updated.id.to_hex());
  }

  // --- transfer -----------------------------------------------------------

  [[nodiscard]] Result<MutationOutcome> apply_transfer(const TransferRequest& request) {
    CanonicalWriter digest_writer;
    digest_writer.u8(static_cast<std::uint8_t>(detail::JournalKind::kMove));
    digest_writer.fixed(request.request_id.span());
    digest_writer.i64(request.now.unix_nanos());
    digest_writer.fixed(request.source.span());
    digest_writer.u64(request.expected_revision.value());
    digest_writer.u8(static_cast<std::uint8_t>(request.mode));
    request.target_holder.encode(digest_writer);
    digest_writer.presence(request.quantity.has_value());
    if (request.quantity.has_value()) {
      digest_writer.u64(request.quantity->units());
      digest_writer.u8(static_cast<std::uint8_t>(request.quantity->unit()));
    }
    digest_writer.u8(static_cast<std::uint8_t>(request.priority.klass));
    digest_writer.u16(request.priority.rank);
    request.actor.encode(digest_writer);
    digest_writer.text(request.note);
    const Sha256Digest request_digest = digest_writer.digest();

    auto validated = require_instant(request.now);
    if (!validated) {
      return validated.error();
    }
    auto actor = require_actor(request.actor, "actor");
    if (!actor) {
      return actor.error();
    }
    if (!is_valid_transfer_mode(request.mode)) {
      return make_error(ErrorCode::kInvalidEnumValue, "transfer mode is not a known mode");
    }
    if (!request.target_holder.is_set()) {
      return make_error(ErrorCode::kMissingField, "transfer requires a target holder tenant");
    }
    const bool minting = request.mode != TransferMode::kMove;
    if (request.mode == TransferMode::kMove && request.quantity.has_value()) {
      return make_error(ErrorCode::kUnexpectedField,
                        "a move transfers the whole remaining quantity; quantity must be omitted");
    }
    if (minting && !request.quantity.has_value()) {
      return make_error(ErrorCode::kMissingField, "split and delegate require an explicit quantity");
    }
    if (minting && (request.quantity->is_zero() || !is_valid_unit(request.quantity->unit()))) {
      return make_error(ErrorCode::kInvalidQuantity, "transferred quantity must be greater than zero");
    }
    if (!is_valid_priority_class(request.priority.klass)) {
      return make_error(ErrorCode::kInvalidPriority, "transfer priority class is not valid");
    }
    Error conflict{};
    if (auto replay = find_idempotent(request.request_id, request_digest, &conflict)) {
      return replay.value();
    }
    if (!conflict.message().empty()) {
      return conflict;
    }
    auto found = require_record(request.source, request.expected_revision);
    if (!found) {
      return found.error();
    }
    const Entitlement& current = *found.value();
    auto live = require_live(current, request.now);
    if (!live) {
      return live.error();
    }
    if (request.target_holder == current.holder) {
      return make_error(ErrorCode::kSelfReference, "the target holder already holds this entitlement");
    }

    detail::StateCore next = state;
    auto sequence = next_sequence(next.commit_seq);
    if (!sequence) {
      return sequence.error();
    }
    next.commit_seq = sequence.value();

    Entitlement source = current;
    std::vector<Entitlement> post_images;
    std::vector<EntitlementId> affected;
    EntitlementId primary = source.id;
    std::string detail;

    if (request.mode == TransferMode::kMove) {
      auto revision = next_revision(source.revision);
      if (!revision) {
        return revision.error();
      }
      HolderChange change;
      change.from = source.holder;
      change.to = request.target_holder;
      change.at = request.now;
      change.commit_seq = next.commit_seq;
      change.actor = request.actor;
      change.note = request.note;
      source.holder = request.target_holder;
      source.holder_history.push_back(change);
      source.revision = revision.value();
      source.updated_at = request.now;
      source.last_commit_seq = next.commit_seq;
      if (!request.note.empty()) {
        source.note = request.note;
      }
      next.insert_or_replace(source);
      post_images.push_back(source);
      affected.push_back(source.id);
      detail = "moved " + source.remaining.to_string() + " from " + change.from.str() + " to " +
               change.to.str();
    } else {
      if (compare_priority(request.priority, source.priority) < 0) {
        return make_error(ErrorCode::kInvalidPriority,
                          "transferred authority may not be more important than its source");
      }
      const Quantity quantity = request.quantity.value();
      if (quantity.unit() != source.unit) {
        return make_error(ErrorCode::kUnitMismatch, "transferred quantity unit disagrees with the source unit");
      }
      if (quantity.units() > source.remaining.units()) {
        return make_error(ErrorCode::kQuantityExceeded,
                          "transferred quantity exceeds the source's remaining authority",
                          "requested=" + quantity.to_string() + " remaining=" + source.remaining.to_string());
      }
      auto ordinal = next_mint(next.mint_ordinal);
      if (!ordinal) {
        return ordinal.error();
      }
      auto identifier = mint_identity(request.request_id, ordinal.value());
      if (!identifier) {
        return identifier.error();
      }
      next.mint_ordinal = ordinal.value();

      Entitlement child;
      child.id = identifier.value();
      child.root = source.root;
      child.relation = (request.mode == TransferMode::kSplit) ? LineageRelation::kSplitFrom
                                                             : LineageRelation::kDelegatedFrom;
      child.sources = {source.id};
      child.revision = Revision::from_value(1u);
      child.mint_ordinal = ordinal.value();
      child.state = EntitlementState::kActive;
      child.holder = request.target_holder;
      child.origin_holder = request.target_holder;
      child.service = source.service;
      child.service_class = source.service_class;
      child.scope = source.scope;
      child.unit = source.unit;
      child.granted = quantity;
      child.remaining = quantity;
      child.delegated_out = Quantity::make(source.unit, 0u).value();
      child.priority = request.priority;
      child.effective_from = source.effective_from;
      child.expires_at = source.expires_at;
      child.created_at = request.now;
      child.updated_at = request.now;
      child.created_by = request.actor;
      child.binding = source.binding;
      child.fence = source.fence;
      child.request_digest = request_digest;
      child.note = request.note;
      child.terminal_reason = TerminalReason::kNotApplicable;
      child.last_commit_seq = next.commit_seq;

      auto source_sequence = next.commit_seq;
      auto revision = next_revision(source.revision);
      if (!revision) {
        return revision.error();
      }
      auto granted = Quantity::checked_sub(source.granted, quantity);
      auto remaining = Quantity::checked_sub(source.remaining, quantity);
      if (!granted.has_value() || !remaining.has_value()) {
        return make_error(ErrorCode::kArithmeticUnderflow,
                          "transfer arithmetic would take the source below zero");
      }
      if (request.mode == TransferMode::kSplit) {
        source.granted = granted.value();
      } else {
        auto delegated = Quantity::checked_add(source.delegated_out, quantity);
        if (!delegated.has_value()) {
          return make_error(ErrorCode::kArithmeticOverflow, "delegated total would overflow");
        }
        source.delegated_out = delegated.value();
      }
      source.remaining = remaining.value();
      source.revision = revision.value();
      source.updated_at = request.now;
      source.last_commit_seq = source_sequence;
      if (!request.note.empty()) {
        source.note = request.note;
      }

      next.insert_or_replace(source);
      next.insert_or_replace(child);
      post_images.push_back(source);
      post_images.push_back(child);
      affected.push_back(source.id);
      affected.push_back(child.id);
      primary = child.id;
      detail = std::string(transfer_mode_name(request.mode)) + " " + child.remaining.to_string() + " to " +
               child.holder.str();
    }

    MutationOutcome outcome = make_outcome(request.request_id, request_digest, request.now,
                                           std::string(transfer_mode_name(request.mode)) + " committed");
    outcome.primary_id = primary;
    outcome.primary_revision = (primary == source.id) ? source.revision : Revision::from_value(1u);
    outcome.affected = affected;
    const EventKind event_kind = (request.mode == TransferMode::kMove)     ? EventKind::kMoved
                                 : (request.mode == TransferMode::kSplit)  ? EventKind::kSplit
                                                                          : EventKind::kDelegated;
    return commit_entry(static_cast<std::uint8_t>(detail::JournalKind::kMove),
                        outcome, std::move(next), std::move(post_images), false, event_kind, detail);
  }

  // --- merge --------------------------------------------------------------

  [[nodiscard]] Result<MutationOutcome> apply_merge(const MergeRequest& request) {
    CanonicalWriter digest_writer;
    digest_writer.u8(static_cast<std::uint8_t>(detail::JournalKind::kMerge));
    digest_writer.fixed(request.request_id.span());
    digest_writer.i64(request.now.unix_nanos());
    digest_writer.fixed(request.first.span());
    digest_writer.u64(request.first_expected_revision.value());
    digest_writer.fixed(request.second.span());
    digest_writer.u64(request.second_expected_revision.value());
    request.actor.encode(digest_writer);
    digest_writer.text(request.note);
    const Sha256Digest request_digest = digest_writer.digest();

    auto validated = require_instant(request.now);
    if (!validated) {
      return validated.error();
    }
    auto actor = require_actor(request.actor, "actor");
    if (!actor) {
      return actor.error();
    }
    if (request.first == request.second) {
      return make_error(ErrorCode::kSelfReference, "an entitlement cannot be merged with itself");
    }
    Error conflict{};
    if (auto replay = find_idempotent(request.request_id, request_digest, &conflict)) {
      return replay.value();
    }
    if (!conflict.message().empty()) {
      return conflict;
    }
    auto first_found = require_record(request.first, request.first_expected_revision);
    if (!first_found) {
      return first_found.error();
    }
    auto second_found = require_record(request.second, request.second_expected_revision);
    if (!second_found) {
      return second_found.error();
    }
    const Entitlement& first = *first_found.value();
    const Entitlement& second = *second_found.value();
    auto first_live = require_live(first, request.now);
    if (!first_live) {
      return first_live.error();
    }
    auto second_live = require_live(second, request.now);
    if (!second_live) {
      return second_live.error();
    }
    if (!(first.holder == second.holder)) {
      return make_error(ErrorCode::kTenantMismatch, "merged entitlements must have the same holder");
    }
    if (!(first.service == second.service)) {
      return make_error(ErrorCode::kServiceMismatch, "merged entitlements must be for the same service");
    }
    if (!(first.service_class == second.service_class)) {
      return make_error(ErrorCode::kServiceClassMismatch,
                        "merged entitlements must have the same service class");
    }
    if (!scope_matches(first.scope, second.scope)) {
      return make_error(ErrorCode::kScopeMismatch, "merged entitlements must cover the same resource scope");
    }
    if (first.unit != second.unit) {
      return make_error(ErrorCode::kUnitMismatch, "merged entitlements must use the same unit");
    }
    if (first.priority != second.priority) {
      return make_error(ErrorCode::kInvalidPriority, "merged entitlements must declare the same priority");
    }
    if (!(first.fence == second.fence)) {
      return make_error(ErrorCode::kInvalidFenceMask, "merged entitlements must declare the same fence mask");
    }
    // Merged authority must come from the same authoritative generations. The
    // per-grant admission evidence is deliberately not required to be equal:
    // each source keeps its own justification, and both remain reachable
    // through the lineage view, so no evidence is discarded.
    const auto same_authority = [](const AuthorityBinding& a, const AuthorityBinding& b) {
      return a.facility_capacity_generation == b.facility_capacity_generation &&
             a.facility_policy_revision == b.facility_policy_revision &&
             a.resource_envelope_revision == b.resource_envelope_revision &&
             a.service_class_revision == b.service_class_revision && a.control_epoch == b.control_epoch &&
             a.policy_context_digest == b.policy_context_digest &&
             a.envelope_binding_digest == b.envelope_binding_digest;
    };
    if (!same_authority(first.binding, second.binding)) {
      return make_error(ErrorCode::kStaleBinding,
                        "merged entitlements must be bound to the same authority generations");
    }
    auto total = Quantity::checked_add(first.remaining, second.remaining);
    if (!total.has_value()) {
      return make_error(ErrorCode::kArithmeticOverflow, "merged quantity would overflow");
    }
    const Timestamp effective_from = (first.effective_from < second.effective_from) ? second.effective_from
                                                                                   : first.effective_from;
    const Timestamp expires_at = (first.expires_at < second.expires_at) ? first.expires_at : second.expires_at;
    if (!(effective_from < expires_at)) {
      return make_error(ErrorCode::kWindowViolation,
                        "merged entitlements have no usable common validity window");
    }

    detail::StateCore next = state;
    auto sequence = next_sequence(next.commit_seq);
    if (!sequence) {
      return sequence.error();
    }
    next.commit_seq = sequence.value();
    auto ordinal = next_mint(next.mint_ordinal);
    if (!ordinal) {
      return ordinal.error();
    }
    auto identifier = mint_identity(request.request_id, ordinal.value());
    if (!identifier) {
      return identifier.error();
    }
    next.mint_ordinal = ordinal.value();

    Entitlement merged;
    merged.id = identifier.value();
    merged.root = first.root;
    merged.relation = LineageRelation::kMergedFrom;
    merged.sources = {first.id, second.id};
    merged.revision = Revision::from_value(1u);
    merged.mint_ordinal = ordinal.value();
    merged.state = EntitlementState::kActive;
    merged.holder = first.holder;
    merged.origin_holder = first.origin_holder;
    merged.service = first.service;
    merged.service_class = first.service_class;
    merged.scope = first.scope;
    merged.unit = first.unit;
    merged.granted = total.value();
    merged.remaining = total.value();
    merged.delegated_out = Quantity::make(first.unit, 0u).value();
    merged.priority = first.priority;
    merged.effective_from = effective_from;
    merged.expires_at = expires_at;
    merged.created_at = request.now;
    merged.updated_at = request.now;
    merged.created_by = request.actor;
    merged.binding = first.binding;
    merged.fence = first.fence;
    merged.request_digest = request_digest;
    merged.note = request.note;
    merged.terminal_reason = TerminalReason::kNotApplicable;
    merged.last_commit_seq = next.commit_seq;

    Entitlement first_after = first;
    auto first_revision = next_revision(first_after.revision);
    if (!first_revision) {
      return first_revision.error();
    }
    first_after.revision = first_revision.value();
    first_after.state = EntitlementState::kSuperseded;
    first_after.terminal_reason = TerminalReason::kSupersededByMerge;
    first_after.updated_at = request.now;
    first_after.last_commit_seq = next.commit_seq;

    Entitlement second_after = second;
    auto second_revision = next_revision(second_after.revision);
    if (!second_revision) {
      return second_revision.error();
    }
    second_after.revision = second_revision.value();
    second_after.state = EntitlementState::kSuperseded;
    second_after.terminal_reason = TerminalReason::kSupersededByMerge;
    second_after.updated_at = request.now;
    second_after.last_commit_seq = next.commit_seq;

    next.insert_or_replace(merged);
    next.insert_or_replace(first_after);
    next.insert_or_replace(second_after);

    MutationOutcome outcome = make_outcome(request.request_id, request_digest, request.now,
                                           "merged " + first.remaining.to_string() + " and " +
                                               second.remaining.to_string() + " into " +
                                               merged.remaining.to_string());
    outcome.primary_id = merged.id;
    outcome.primary_revision = merged.revision;
    outcome.affected = {merged.id, first_after.id, second_after.id};
    return commit_entry(static_cast<std::uint8_t>(detail::JournalKind::kMerge), outcome, std::move(next),
                        {merged, first_after, second_after}, false, EventKind::kMerged,
                        merged.id.to_hex() + " from " + first_after.id.to_hex() + " and " +
                            second_after.id.to_hex());
  }

  // --- reissue ------------------------------------------------------------

  [[nodiscard]] Result<MutationOutcome> apply_reissue(const ReissueRequest& request) {
    CanonicalWriter digest_writer;
    digest_writer.u8(static_cast<std::uint8_t>(detail::JournalKind::kReissue));
    digest_writer.fixed(request.request_id.span());
    digest_writer.i64(request.now.unix_nanos());
    digest_writer.fixed(request.id.span());
    digest_writer.u64(request.expected_revision.value());
    digest_writer.boolean(request.rebind_to_current_authority);
    request.actor.encode(digest_writer);
    digest_writer.text(request.note);
    const Sha256Digest request_digest = digest_writer.digest();

    auto validated = require_instant(request.now);
    if (!validated) {
      return validated.error();
    }
    auto actor = require_actor(request.actor, "actor");
    if (!actor) {
      return actor.error();
    }
    Error conflict{};
    if (auto replay = find_idempotent(request.request_id, request_digest, &conflict)) {
      return replay.value();
    }
    if (!conflict.message().empty()) {
      return conflict;
    }
    auto found = require_record(request.id, request.expected_revision);
    if (!found) {
      return found.error();
    }
    const Entitlement& current = *found.value();
    if (is_terminal_state(current.state)) {
      return make_error(ErrorCode::kInvalidTransition, "a terminal entitlement cannot be reissued",
                        entitlement_state_name(current.state));
    }
    if (current.remaining.is_zero()) {
      return make_error(ErrorCode::kQuantityExceeded, "there is no remaining authority to reissue");
    }

    detail::StateCore next = state;
    auto sequence = next_sequence(next.commit_seq);
    if (!sequence) {
      return sequence.error();
    }
    next.commit_seq = sequence.value();
    auto ordinal = next_mint(next.mint_ordinal);
    if (!ordinal) {
      return ordinal.error();
    }
    auto identifier = mint_identity(request.request_id, ordinal.value());
    if (!identifier) {
      return identifier.error();
    }
    next.mint_ordinal = ordinal.value();

    Entitlement replacement;
    replacement.id = identifier.value();
    replacement.root = current.root;
    replacement.relation = LineageRelation::kReissuedFrom;
    replacement.sources = {current.id};
    replacement.revision = Revision::from_value(1u);
    replacement.mint_ordinal = ordinal.value();
    replacement.state = EntitlementState::kActive;
    replacement.holder = current.holder;
    replacement.origin_holder = current.origin_holder;
    replacement.service = current.service;
    replacement.service_class = current.service_class;
    replacement.scope = current.scope;
    replacement.unit = current.unit;
    replacement.granted = current.remaining;
    replacement.remaining = current.remaining;
    replacement.delegated_out = Quantity::make(current.unit, 0u).value();
    replacement.priority = current.priority;
    replacement.effective_from = current.effective_from;
    replacement.expires_at = current.expires_at;
    replacement.created_at = request.now;
    replacement.updated_at = request.now;
    replacement.created_by = request.actor;
    replacement.fence = current.fence;
    replacement.request_digest = request_digest;
    replacement.note = request.note;
    replacement.terminal_reason = TerminalReason::kNotApplicable;
    replacement.last_commit_seq = next.commit_seq;
    replacement.binding = current.binding;
    if (request.rebind_to_current_authority) {
      replacement.binding.authority_revision = next.authority.revision;
      replacement.binding.facility_capacity_generation = next.authority.facility_capacity_generation;
      replacement.binding.facility_policy_revision = next.authority.facility_policy_revision;
      replacement.binding.resource_envelope_revision = next.authority.resource_envelope_revision;
      replacement.binding.service_class_revision = next.authority.service_class_revision;
      replacement.binding.control_epoch = next.authority.control_epoch;
      replacement.binding.policy_context_digest = next.authority.policy_context_digest;
      replacement.binding.envelope_binding_digest = next.authority.envelope_binding_digest;
    }

    Entitlement source = current;
    auto revision = next_revision(source.revision);
    if (!revision) {
      return revision.error();
    }
    source.revision = revision.value();
    source.state = EntitlementState::kSuperseded;
    source.terminal_reason = TerminalReason::kSupersededByReissue;
    source.updated_at = request.now;
    source.last_commit_seq = next.commit_seq;

    next.insert_or_replace(replacement);
    next.insert_or_replace(source);

    MutationOutcome outcome = make_outcome(
        request.request_id, request_digest, request.now,
        std::string("authority reissued as a new record bound to authority revision ") +
            std::to_string(replacement.binding.authority_revision.value()));
    outcome.primary_id = replacement.id;
    outcome.primary_revision = replacement.revision;
    outcome.affected = {replacement.id, source.id};
    return commit_entry(static_cast<std::uint8_t>(detail::JournalKind::kReissue), outcome, std::move(next),
                        {replacement, source}, false, EventKind::kReissued,
                        source.id.to_hex() + " -> " + replacement.id.to_hex());
  }

  // --- accounting ---------------------------------------------------------

  [[nodiscard]] Result<MutationOutcome> apply_draw(const DrawRequest& request) {
    CanonicalWriter digest_writer;
    digest_writer.u8(static_cast<std::uint8_t>(detail::JournalKind::kDraw));
    digest_writer.fixed(request.request_id.span());
    digest_writer.i64(request.now.unix_nanos());
    digest_writer.fixed(request.id.span());
    digest_writer.u64(request.expected_revision.value());
    digest_writer.u64(request.quantity.units());
    digest_writer.u8(static_cast<std::uint8_t>(request.quantity.unit()));
    request.actor.encode(digest_writer);
    digest_writer.text(request.note);
    const Sha256Digest request_digest = digest_writer.digest();

    auto validated = require_instant(request.now);
    if (!validated) {
      return validated.error();
    }
    auto actor = require_actor(request.actor, "actor");
    if (!actor) {
      return actor.error();
    }
    if (request.quantity.is_zero()) {
      return make_error(ErrorCode::kInvalidQuantity, "draw quantity must be greater than zero");
    }
    Error conflict{};
    if (auto replay = find_idempotent(request.request_id, request_digest, &conflict)) {
      return replay.value();
    }
    if (!conflict.message().empty()) {
      return conflict;
    }
    auto found = require_record(request.id, request.expected_revision);
    if (!found) {
      return found.error();
    }
    const Entitlement& current = *found.value();
    const LivenessView view = evaluate_liveness(current, state.authority, request.now);
    if (!view.live) {
      return make_error(view.primary, "the entitlement does not confer live authority for this draw",
                        view.explanation);
    }
    if (request.quantity.unit() != current.unit) {
      return make_error(ErrorCode::kUnitMismatch, "draw quantity unit disagrees with the entitlement unit");
    }
    auto remaining = Quantity::checked_sub(current.remaining, request.quantity);
    if (!remaining.has_value()) {
      return make_error(ErrorCode::kQuantityExceeded, "draw exceeds the remaining authority",
                        "requested=" + request.quantity.to_string() +
                            " remaining=" + current.remaining.to_string());
    }

    detail::StateCore next = state;
    auto sequence = next_sequence(next.commit_seq);
    if (!sequence) {
      return sequence.error();
    }
    next.commit_seq = sequence.value();
    Entitlement updated = current;
    auto revision = next_revision(updated.revision);
    if (!revision) {
      return revision.error();
    }
    updated.revision = revision.value();
    updated.remaining = remaining.value();
    updated.updated_at = request.now;
    updated.last_commit_seq = next.commit_seq;
    next.insert_or_replace(updated);

    MutationOutcome outcome = make_outcome(request.request_id, request_digest, request.now,
                                           "drew " + request.quantity.to_string() + "; " +
                                               updated.remaining.to_string() + " remains");
    outcome.primary_id = updated.id;
    outcome.primary_revision = updated.revision;
    outcome.affected = {updated.id};
    return commit_entry(static_cast<std::uint8_t>(detail::JournalKind::kDraw), outcome, std::move(next),
                        {updated}, false, EventKind::kDrawn,
                        updated.id.to_hex() + " remaining=" + std::to_string(updated.remaining.units()));
  }

  [[nodiscard]] Result<MutationOutcome> apply_release(const ReleaseRequest& request) {
    CanonicalWriter digest_writer;
    digest_writer.u8(static_cast<std::uint8_t>(detail::JournalKind::kRelease));
    digest_writer.fixed(request.request_id.span());
    digest_writer.i64(request.now.unix_nanos());
    digest_writer.fixed(request.id.span());
    digest_writer.u64(request.expected_revision.value());
    digest_writer.u64(request.quantity.units());
    digest_writer.u8(static_cast<std::uint8_t>(request.quantity.unit()));
    request.actor.encode(digest_writer);
    digest_writer.text(request.note);
    const Sha256Digest request_digest = digest_writer.digest();

    auto validated = require_instant(request.now);
    if (!validated) {
      return validated.error();
    }
    auto actor = require_actor(request.actor, "actor");
    if (!actor) {
      return actor.error();
    }
    if (request.quantity.is_zero()) {
      return make_error(ErrorCode::kInvalidQuantity, "release quantity must be greater than zero");
    }
    Error conflict{};
    if (auto replay = find_idempotent(request.request_id, request_digest, &conflict)) {
      return replay.value();
    }
    if (!conflict.message().empty()) {
      return conflict;
    }
    auto found = require_record(request.id, request.expected_revision);
    if (!found) {
      return found.error();
    }
    const Entitlement& current = *found.value();
    if (is_terminal_state(current.state)) {
      return make_error(ErrorCode::kInvalidTransition, "a terminal entitlement cannot be released into",
                        entitlement_state_name(current.state));
    }
    if (request.quantity.unit() != current.unit) {
      return make_error(ErrorCode::kUnitMismatch, "release quantity unit disagrees with the entitlement unit");
    }
    auto remaining = Quantity::checked_add(current.remaining, request.quantity);
    if (!remaining.has_value()) {
      return make_error(ErrorCode::kArithmeticOverflow, "release would overflow the remaining quantity");
    }
    if (remaining.value().units() > current.granted.units()) {
      return make_error(ErrorCode::kLimitExceeded,
                        "release would return more authority than was ever granted",
                        "granted=" + current.granted.to_string() +
                            " resulting=" + remaining.value().to_string());
    }

    detail::StateCore next = state;
    auto sequence = next_sequence(next.commit_seq);
    if (!sequence) {
      return sequence.error();
    }
    next.commit_seq = sequence.value();
    Entitlement updated = current;
    auto revision = next_revision(updated.revision);
    if (!revision) {
      return revision.error();
    }
    updated.revision = revision.value();
    updated.remaining = remaining.value();
    updated.updated_at = request.now;
    updated.last_commit_seq = next.commit_seq;
    next.insert_or_replace(updated);

    MutationOutcome outcome = make_outcome(request.request_id, request_digest, request.now,
                                           "released " + request.quantity.to_string() + "; " +
                                               updated.remaining.to_string() + " available");
    outcome.primary_id = updated.id;
    outcome.primary_revision = updated.revision;
    outcome.affected = {updated.id};
    return commit_entry(static_cast<std::uint8_t>(detail::JournalKind::kRelease), outcome, std::move(next),
                        {updated}, false, EventKind::kReleased,
                        updated.id.to_hex() + " remaining=" + std::to_string(updated.remaining.units()));
  }

  // --- reads --------------------------------------------------------------

  [[nodiscard]] VerificationDecision decide(const EntitlementId& id, const Timestamp& now,
                                            const std::optional<ExpectedContext>& expected,
                                            const std::optional<Quantity>& requested,
                                            const Sha256Digest& token_digest) const {
    VerificationDecision decision;
    decision.id = id;
    decision.decision_time = now;
    decision.observed_commit_seq = state.commit_seq;
    decision.control_epoch = state.authority.control_epoch;
    decision.token_digest = token_digest;
    const Entitlement* record = state.find(id);
    if (record == nullptr) {
      decision.authorized = false;
      decision.primary = ErrorCode::kUnknownEntitlement;
      decision.explanation = "no entitlement with that identity exists in the ledger";
      return decision;
    }
    decision.revision = record->revision;
    decision.remaining = record->remaining;
    decision.binding_digest = record->binding.binding_digest();

    std::vector<ErrorCode> faults;
    const LivenessView live = evaluate_liveness(*record, state.authority, now);
    for (const ErrorCode fault : live.faults) {
      append_fault(faults, fault);
    }
    decision.stale = live.stale;
    if (expected.has_value()) {
      if (!(record->holder == expected->holder)) {
        append_fault(faults, ErrorCode::kTenantMismatch);
      }
      if (!(record->service == expected->service)) {
        append_fault(faults, ErrorCode::kServiceMismatch);
      }
      if (!(record->service_class == expected->service_class)) {
        append_fault(faults, ErrorCode::kServiceClassMismatch);
      }
      if (!scope_matches(record->scope, expected->scope)) {
        append_fault(faults, ErrorCode::kScopeMismatch);
      }
      if (record->unit != expected->unit) {
        append_fault(faults, ErrorCode::kUnitMismatch);
      }
    }
    if (requested.has_value()) {
      if (requested->unit() != record->unit) {
        append_fault(faults, ErrorCode::kUnitMismatch);
      } else if (requested->units() > record->remaining.units()) {
        append_fault(faults, ErrorCode::kQuantityExceeded);
      }
    }

    if (faults.empty()) {
      decision.authorized = true;
      decision.primary = ErrorCode::kOk;
      decision.explanation = "live authority for " + record->remaining.to_string() + " of scope " +
                             record->scope.scope.str() + " at facility " + record->scope.facility.str();
      return decision;
    }
    decision.authorized = false;
    decision.primary = faults.front();
    std::string explanation = std::string("refused: ") + error_code_name(faults.front());
    for (std::size_t i = 1; i < faults.size(); ++i) {
      decision.secondary_faults.push_back(faults[i]);
      explanation.append("; also ");
      explanation.append(error_code_name(faults[i]));
    }
    if (requested.has_value()) {
      explanation.append(" [requested=");
      explanation.append(requested->to_string());
      explanation.append("]");
    }
    decision.explanation = explanation;
    return decision;
  }

  [[nodiscard]] Result<OrderResult> order(const OrderRequest& request) const {
    OrderResult result;
    std::vector<OrderEntry> entries;
    for (const Entitlement& record : state.entitlements) {
      if (request.holder.has_value() && !(record.holder == request.holder.value())) {
        continue;
      }
      if (request.scope.has_value() && !scope_matches(record.scope, request.scope.value())) {
        continue;
      }
      if (request.unit.has_value() && record.unit != request.unit.value()) {
        continue;
      }
      const LivenessView live = evaluate_liveness(record, state.authority, request.now);
      if (!live.live) {
        ExcludedEntry excluded;
        excluded.id = record.id;
        excluded.code = live.primary;
        excluded.explanation = live.explanation;
        result.excluded.push_back(std::move(excluded));
        continue;
      }
      OrderEntry entry;
      entry.id = record.id;
      entry.priority = record.priority;
      entry.remaining = record.remaining;
      entry.effective_from = record.effective_from;
      entry.expires_at = record.expires_at;
      entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const OrderEntry& a, const OrderEntry& b) {
      const int priority = compare_priority(a.priority, b.priority);
      if (priority != 0) {
        return priority < 0;
      }
      if (!(a.effective_from == b.effective_from)) {
        return a.effective_from < b.effective_from;
      }
      if (!(a.expires_at == b.expires_at)) {
        return a.expires_at < b.expires_at;
      }
      return a.id < b.id;
    });
    result.entries = std::move(entries);
    return result;
  }

  void refresh_derived_stats() {
    stats.commit_seq = state.commit_seq;
    stats.authority_revision = state.authority.revision;
    stats.control_epoch = state.authority.control_epoch;
    stats.mint_ordinal = state.mint_ordinal;
    stats.entitlement_count = state.entitlements.size();
    stats.idempotency_entries = state.idempotency.size();
    std::size_t terminal = 0;
    std::size_t live = 0;
    for (const Entitlement& record : state.entitlements) {
      if (is_terminal_state(record.state)) {
        ++terminal;
      }
      if (!record.remaining.is_zero() && !is_terminal_state(record.state)) {
        ++live;
      }
    }
    stats.terminal_count = terminal;
    stats.live_count = live;
  }

  [[nodiscard]] Result<void> fence_on_open() {
    bool any = false;
    for (const Entitlement& record : state.entitlements) {
      if (!is_terminal_state(record.state)) {
        any = true;
        break;
      }
    }
    if (!any) {
      return {};
    }
    detail::StateCore next = state;
    auto epoch = next_epoch(next.authority.control_epoch);
    if (!epoch) {
      return epoch.error();
    }
    auto revision = next_revision(next.authority.revision);
    if (!revision) {
      return revision.error();
    }
    auto sequence = next_sequence(next.commit_seq);
    if (!sequence) {
      return sequence.error();
    }
    const Epoch previous_epoch = next.authority.control_epoch;
    next.authority.control_epoch = epoch.value();
    next.authority.revision = revision.value();
    next.commit_seq = sequence.value();

    MutationOutcome outcome;
    outcome.code = ErrorCode::kOk;
    outcome.request_id =
        RequestId::derive("fence-on-open:" + std::to_string(next.commit_seq.value()));
    outcome.request_digest = Sha256::hash(std::string_view("fence-on-open"));
    outcome.committed_at = next.authority.published_at;
    outcome.message = "control epoch advanced from " + std::to_string(previous_epoch.value()) + " to " +
                      std::to_string(next.authority.control_epoch.value()) +
                      " while opening; all authority granted under the previous epoch is fenced";
    auto committed = commit_entry(static_cast<std::uint8_t>(detail::JournalKind::kFenceOnOpen), outcome,
                                  std::move(next), {}, true, EventKind::kFencedOnOpen,
                                  "epoch=" + std::to_string(previous_epoch.value()) + "->" +
                                      std::to_string(state.authority.control_epoch.value()));
    if (!committed) {
      return committed.error();
    }
    return {};
  }

  template <class Fn>
  [[nodiscard]] Result<MutationOutcome> mutate(Fn&& body) {
    std::vector<Event> events;
    MutationOutcome outcome;
    {
      std::unique_lock<std::shared_mutex> guard(mutex);
      if (read_only) {
        return make_error(ErrorCode::kReadOnlyStore, "this store was opened read-only");
      }
      pending_events.clear();
      auto result = body();
      if (!result) {
        pending_events.clear();
        return result.error();
      }
      outcome = std::move(result.value());
      events.swap(pending_events);
    }
    emit(events);
    return outcome;
  }
};

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

Store::Store(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Store::~Store() = default;

Result<std::unique_ptr<Store>> Store::open(const StoreOpenOptions& options) {
  auto valid = detail::validate_options(options);
  if (!valid) {
    return valid.error();
  }
  auto impl = std::make_unique<Impl>();
  impl->options = options;
  impl->read_only = options.read_only;

  if (options.read_only) {
    auto loaded = detail::DurableStore::open_read_only(options);
    if (!loaded) {
      return loaded.error();
    }
    impl->state = std::move(loaded.value().state);
    impl->open_report = loaded.value().report;
    impl->open_message = loaded.value().report.message;
  } else {
    auto loaded = detail::DurableStore::open(options);
    if (!loaded) {
      return loaded.error();
    }
    impl->state = std::move(loaded.value().state);
    impl->open_report = loaded.value().report;
    impl->open_message = loaded.value().report.message;
    impl->durable = std::move(loaded.value().store);
    auto entries = detail::list_directory(options.directory);
    if (entries.has_value()) {
      for (const std::filesystem::path& entry : entries.value()) {
        std::uint32_t index = 0;
        if (detail::parse_segment_name(entry.filename().string(), index)) {
          impl->log_segment_count += 1u;
          const auto size = detail::file_size_bytes(options.directory / entry.filename());
          if (size.has_value()) {
            impl->open_log_bytes += size.value();
          }
        }
      }
    }
    if (options.fence_live_authority_on_open) {
      auto fenced = impl->fence_on_open();
      if (!fenced) {
        return fenced.error();
      }
    }
  }

  impl->refresh_derived_stats();
  impl->stats.commit_seq = impl->state.commit_seq;
  impl->stats.log_segment_count = impl->durable != nullptr ? impl->durable->segment_count()
                                                           : impl->log_segment_count;
  impl->stats.log_bytes = impl->open_log_bytes;
  impl->stats.bytes_written = impl->durable != nullptr ? impl->durable->bytes_written() : 0u;
  impl->stats.fence_write_failures = impl->durable != nullptr ? impl->durable->fence_write_failures() : 0u;

  Event opened;
  opened.kind = EventKind::kStoreOpened;
  opened.commit_seq = impl->state.commit_seq;
  opened.at = impl->state.authority.published_at;
  opened.code = ErrorCode::kOk;
  opened.detail = options.read_only ? "opened read-only" : "opened for writing";
  if (impl->options.event_sink) {
    impl->options.event_sink(opened);
  }
  return std::unique_ptr<Store>(new Store(std::move(impl)));
}

bool Store::is_read_only() const noexcept { return impl_->read_only; }

const std::filesystem::path& Store::directory() const noexcept { return impl_->options.directory; }

AuthoritySnapshot Store::authority() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.authority;
}

Sequence Store::commit_seq() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  return impl_->state.commit_seq;
}

StoreStats Store::stats() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  impl_->refresh_derived_stats();
  StoreStats copy = impl_->stats;
  copy.log_segment_count = impl_->durable != nullptr ? impl_->durable->segment_count()
                                                     : impl_->log_segment_count;
  copy.log_bytes = impl_->open_log_bytes +
                   (impl_->durable != nullptr ? impl_->durable->bytes_written() : 0u);
  copy.bytes_written = impl_->durable != nullptr ? impl_->durable->bytes_written() : 0u;
  copy.fence_write_failures = impl_->durable != nullptr ? impl_->durable->fence_write_failures() : 0u;
  return copy;
}
Result<MutationOutcome> Store::update_authority(const AuthorityUpdateRequest& request) {
  return impl_->mutate([this, &request] { return impl_->apply_authority_update(request); });
}

Result<MutationOutcome> Store::grant(const GrantRequest& request) {
  return impl_->mutate([this, &request] { return impl_->apply_grant(request); });
}

Result<MutationOutcome> Store::suspend(const SuspendRequest& request) {
  return impl_->mutate([this, &request] { return impl_->apply_suspend(request); });
}

Result<MutationOutcome> Store::resume(const ResumeRequest& request) {
  return impl_->mutate([this, &request] { return impl_->apply_resume(request); });
}

Result<MutationOutcome> Store::revoke(const RevokeRequest& request) {
  return impl_->mutate([this, &request] { return impl_->apply_revoke(request); });
}

Result<MutationOutcome> Store::expire(const ExpireRequest& request) {
  return impl_->mutate([this, &request] { return impl_->apply_expire(request); });
}

Result<MutationOutcome> Store::transfer(const TransferRequest& request) {
  return impl_->mutate([this, &request] { return impl_->apply_transfer(request); });
}

Result<MutationOutcome> Store::merge(const MergeRequest& request) {
  return impl_->mutate([this, &request] { return impl_->apply_merge(request); });
}

Result<MutationOutcome> Store::reissue(const ReissueRequest& request) {
  return impl_->mutate([this, &request] { return impl_->apply_reissue(request); });
}

Result<MutationOutcome> Store::draw(const DrawRequest& request) {
  return impl_->mutate([this, &request] { return impl_->apply_draw(request); });
}

Result<MutationOutcome> Store::release(const ReleaseRequest& request) {
  return impl_->mutate([this, &request] { return impl_->apply_release(request); });
}

Result<Entitlement> Store::get(const EntitlementId& id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const Entitlement* record = impl_->state.find(id);
  if (record == nullptr) {
    return Error(ErrorCode::kUnknownEntitlement, "no entitlement with that identity exists in the ledger",
                 id.to_hex());
  }
  return *record;
}

Result<std::vector<Entitlement>> Store::list(const ListFilter& filter) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const std::size_t limit = filter.limit == 0u ? kDefaultListLimit : std::min(filter.limit, kMaxListLimit);
  std::vector<Entitlement> out;
  for (const Entitlement& record : impl_->state.entitlements) {
    if (out.size() >= limit) {
      break;
    }
    if (filter.holder.has_value() && !(record.holder == filter.holder.value())) {
      continue;
    }
    if (filter.service.has_value() && !(record.service == filter.service.value())) {
      continue;
    }
    if (filter.scope.has_value() && !scope_matches(record.scope, filter.scope.value())) {
      continue;
    }
    if (filter.state.has_value() && record.state != filter.state.value()) {
      continue;
    }
    if (filter.relation.has_value() && record.relation != filter.relation.value()) {
      continue;
    }
    if (filter.root.has_value() && !(record.root == filter.root.value())) {
      continue;
    }
    if (filter.derived_from.has_value()) {
      const bool referenced = std::find(record.sources.begin(), record.sources.end(),
                                        filter.derived_from.value()) != record.sources.end();
      if (!referenced) {
        continue;
      }
    }
    if (filter.live_only) {
      const LivenessView live = evaluate_liveness(record, impl_->state.authority, filter.now);
      if (!live.live) {
        continue;
      }
    }
    out.push_back(record);
  }
  return out;
}

Result<LineageView> Store::lineage(const EntitlementId& id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const Entitlement* record = impl_->state.find(id);
  if (record == nullptr) {
    return Error(ErrorCode::kUnknownEntitlement, "no entitlement with that identity exists in the ledger",
                 id.to_hex());
  }
  // The lineage view is the connected component of the derivation graph that
  // contains this record: ancestry through sources, plus every descendant that
  // names a member in its sources. Two independently granted records can be
  // merged, so a single root identity is not enough to describe a lineage.
  const EntitlementId root = record->root;
  std::vector<EntitlementId> component{record->id};
  bool grew = true;
  while (grew) {
    grew = false;
    for (const Entitlement& candidate : impl_->state.entitlements) {
      const bool member = std::find(component.begin(), component.end(), candidate.id) != component.end();
      if (member) {
        for (const EntitlementId& source : candidate.sources) {
          if (std::find(component.begin(), component.end(), source) == component.end()) {
            component.push_back(source);
            grew = true;
          }
        }
        continue;
      }
      for (const EntitlementId& source : candidate.sources) {
        if (std::find(component.begin(), component.end(), source) != component.end()) {
          component.push_back(candidate.id);
          grew = true;
          break;
        }
      }
    }
  }

  LineageView view;
  view.root = root;
  for (const Entitlement& candidate : impl_->state.entitlements) {
    if (std::find(component.begin(), component.end(), candidate.id) == component.end()) {
      continue;
    }
    LineageNode node;
    node.id = candidate.id;
    node.relation = candidate.relation;
    node.sources = candidate.sources;
    node.holder = candidate.holder;
    node.state = candidate.state;
    node.revision = candidate.revision;
    node.created_at = candidate.created_at;
    node.granted = candidate.granted;
    node.remaining = candidate.remaining;
    node.delegated_out = candidate.delegated_out;
    node.revoked_at = candidate.revoked_at;
    node.terminal_reason = candidate.terminal_reason;
    const LivenessView live = evaluate_liveness(candidate, impl_->state.authority, candidate.updated_at);
    node.live = live.live;
    node.liveness_code = live.primary;
    view.nodes.push_back(std::move(node));
    for (const HolderChange& change : candidate.holder_history) {
      view.holder_history.push_back(change);
    }
  }
  std::sort(view.nodes.begin(), view.nodes.end(), [](const LineageNode& a, const LineageNode& b) {
    if (!(a.created_at == b.created_at)) {
      return a.created_at < b.created_at;
    }
    return a.id < b.id;
  });
  std::sort(view.holder_history.begin(), view.holder_history.end(),
            [](const HolderChange& a, const HolderChange& b) {
              if (!(a.commit_seq == b.commit_seq)) {
                return a.commit_seq < b.commit_seq;
              }
              return a.at < b.at;
            });
  return view;
}

Result<OrderResult> Store::order_for_consumption(const OrderRequest& request) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  if (request.now.is_zero()) {
    return Error(ErrorCode::kInvalidTimestamp, "ordering requires an explicit decision instant");
  }
  return impl_->order(request);
}

Result<VerificationDecision> Store::verify(const VerifyRequest& request) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  if (request.now.is_zero()) {
    return Error(ErrorCode::kInvalidTimestamp,
                 "verification requires an explicit decision instant; the zero instant is not accepted");
  }
  return impl_->decide(request.id, request.now, request.expected, request.requested, Sha256Digest{});
}

Result<VerificationDecision> Store::verify_token(const VerifyTokenRequest& request) const {
  auto decoded = EntitlementToken::decode(request.token);
  if (!decoded) {
    return decoded.error();
  }
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  if (request.now.is_zero()) {
    return Error(ErrorCode::kInvalidTimestamp,
                 "verification requires an explicit decision instant; the zero instant is not accepted");
  }
  const EntitlementToken& token = decoded.value();
  const EntitlementId id = token.id();
  const Sha256Digest token_digest = token.token_digest();
  const Entitlement* stored = impl_->state.find(id);
  if (stored == nullptr) {
    VerificationDecision decision;
    decision.id = id;
    decision.decision_time = request.now;
    decision.observed_commit_seq = impl_->state.commit_seq;
    decision.control_epoch = impl_->state.authority.control_epoch;
    decision.token_digest = token_digest;
    decision.primary = ErrorCode::kUnknownEntitlement;
    decision.explanation = "the token names an entitlement that does not exist in the ledger";
    return decision;
  }
  if (!(stored->revision == token.record().revision)) {
    VerificationDecision decision;
    decision.id = id;
    decision.decision_time = request.now;
    decision.observed_commit_seq = impl_->state.commit_seq;
    decision.control_epoch = impl_->state.authority.control_epoch;
    decision.token_digest = token_digest;
    decision.revision = stored->revision;
    decision.remaining = stored->remaining;
    decision.binding_digest = stored->binding.binding_digest();
    decision.stale = true;
    decision.primary = ErrorCode::kStaleBinding;
    decision.explanation = "the token describes revision " +
                           std::to_string(token.record().revision.value()) +
                           " but the ledger holds revision " + std::to_string(stored->revision.value());
    return decision;
  }
  return impl_->decide(id, request.now, request.expected, request.requested, token_digest);
}

Result<EntitlementToken> Store::issue_token(const EntitlementId& id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  const Entitlement* record = impl_->state.find(id);
  if (record == nullptr) {
    return Error(ErrorCode::kUnknownEntitlement, "no entitlement with that identity exists in the ledger",
                 id.to_hex());
  }
  return EntitlementToken::issue(*record);
}

Result<MutationOutcome> Store::lookup_outcome(const RequestId& request_id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex);
  for (auto it = impl_->state.idempotency.rbegin(); it != impl_->state.idempotency.rend(); ++it) {
    if (it->request_id == request_id) {
      MutationOutcome outcome = it->outcome;
      outcome.replayed = true;
      return outcome;
    }
  }
  return Error(ErrorCode::kNoRecordedOutcome,
               "no committed outcome for that request identity is inside the idempotency window",
               request_id.to_hex());
}

Result<CompactionReport> Store::compact() {
  std::unique_lock<std::shared_mutex> guard(impl_->mutex);
  if (impl_->read_only) {
    return Error(ErrorCode::kReadOnlyStore, "this store was opened read-only");
  }
  auto report = impl_->durable->write_snapshot(impl_->state);
  if (!report) {
    return report.error();
  }
  impl_->queue_event(EventKind::kCompacted, impl_->state.commit_seq, impl_->state.authority.published_at,
                     EntitlementId{}, impl_->state.authority.revision, ErrorCode::kOk,
                     report.value().message);
  std::vector<Event> events;
  events.swap(impl_->pending_events);
  guard.unlock();
  impl_->emit(events);
  return report.value();
}

Result<InspectionReport> Store::inspect(const std::filesystem::path& directory) {
  StoreOpenOptions options;
  options.directory = directory;
  options.read_only = true;
  auto loaded = detail::DurableStore::open_read_only(options);
  if (!loaded) {
    return loaded.error();
  }
  InspectionReport report = loaded.value().report;
  report.opened = true;
  auto lock = detail::WriterLock::acquire(detail::lock_path(directory));
  report.writer_lock_free = lock.has_value();
  return report;
}

}  // namespace entl
