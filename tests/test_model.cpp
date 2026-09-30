// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <optional>
#include <string>
#include <vector>

#include "harness.hpp"
#include "resource_entitlement/binding.hpp"
#include "resource_entitlement/entitlement.hpp"
#include "resource_entitlement/priority.hpp"
#include "resource_entitlement/quantity.hpp"
#include "resource_entitlement/request.hpp"
#include "resource_entitlement/token.hpp"
#include "test_support.hpp"

using entl::Entitlement;
using entl::EntitlementState;
using entl::ErrorCode;
using entl::Priority;
using entl::PriorityClass;
using entl::Quantity;
using entl::Timestamp;
using entl::Unit;

namespace {

entl::AuthoritySnapshot matching_snapshot() {
  entl::AuthoritySnapshot snapshot;
  snapshot.revision = entl::Revision::from_value(3u);
  snapshot.facility_capacity_generation = entl::Generation::from_value(5u);
  snapshot.facility_policy_revision = entl::Revision::from_value(2u);
  snapshot.resource_envelope_revision = entl::Revision::from_value(4u);
  snapshot.service_class_revision = entl::Revision::from_value(6u);
  snapshot.control_epoch = entl::Epoch::from_value(9u);
  snapshot.policy_context_digest = re_test::digest_of("policy");
  snapshot.envelope_binding_digest = re_test::digest_of("envelope");
  snapshot.published_at = re_test::instant(1700000000);
  snapshot.publisher = re_test::must_parse<entl::ActorId>("facility");
  return snapshot;
}

entl::AuthorityBinding binding_for(const entl::AuthoritySnapshot& snapshot) {
  entl::AuthorityBinding binding;
  binding.authority_revision = snapshot.revision;
  binding.facility_capacity_generation = snapshot.facility_capacity_generation;
  binding.facility_policy_revision = snapshot.facility_policy_revision;
  binding.resource_envelope_revision = snapshot.resource_envelope_revision;
  binding.service_class_revision = snapshot.service_class_revision;
  binding.control_epoch = snapshot.control_epoch;
  binding.policy_context_digest = snapshot.policy_context_digest;
  binding.envelope_binding_digest = snapshot.envelope_binding_digest;
  binding.admission_decision_digest = re_test::digest_of("admission");
  return binding;
}

Entitlement make_record() {
  const entl::AuthoritySnapshot snapshot = matching_snapshot();
  Entitlement record;
  record.id = entl::EntitlementId::derive("record-identity");
  record.root = record.id;
  record.relation = entl::LineageRelation::kRoot;
  record.revision = entl::Revision::from_value(1u);
  record.mint_ordinal = entl::MintOrdinal::from_value(1u);
  record.state = EntitlementState::kActive;
  record.holder = re_test::must_parse<entl::TenantId>("tenant-a");
  record.origin_holder = record.holder;
  record.service = re_test::must_parse<entl::ServiceId>("inference");
  record.service_class = re_test::must_parse<entl::ServiceClassId>("gold");
  record.scope.facility = re_test::must_parse<entl::FacilityId>("dc-1");
  record.scope.resource_type = re_test::must_parse<entl::ResourceTypeId>("accelerator");
  record.scope.scope = re_test::must_parse<entl::ResourceScopeId>("pool-a");
  record.unit = Unit::kCount;
  record.granted = Quantity::make(Unit::kCount, 100u).value();
  record.remaining = Quantity::make(Unit::kCount, 100u).value();
  record.delegated_out = Quantity::make(Unit::kCount, 0u).value();
  record.priority.klass = PriorityClass::kStandard;
  record.priority.rank = 4u;
  record.effective_from = re_test::instant(1699999000);
  record.expires_at = re_test::instant(1799999000);
  record.created_at = re_test::instant(1699999000);
  record.updated_at = re_test::instant(1699999000);
  record.created_by = re_test::must_parse<entl::ActorId>("operator-1");
  record.binding = binding_for(snapshot);
  record.fence = entl::FenceMask::standard();
  record.request_digest = re_test::digest_of("grant-request");
  record.last_commit_seq = entl::Sequence::from_value(4u);
  return record;
}

}  // namespace

RE_TEST(model, quantity_arithmetic_is_checked) {
  const auto units = Quantity::make(Unit::kCount, 7u).value();
  const auto other = Quantity::make(Unit::kCount, 5u).value();
  const auto gib = Quantity::make(Unit::kGibibytes, 5u).value();

  RE_CHECK_EQ(Quantity::checked_add(units, other).value().units(), std::uint64_t{12});
  RE_CHECK_EQ(Quantity::checked_sub(units, other).value().units(), std::uint64_t{2});
  RE_CHECK(!Quantity::checked_sub(other, units).has_value());
  RE_CHECK(!Quantity::checked_add(units, gib).has_value());
  RE_CHECK(!Quantity::make(static_cast<Unit>(0u), 1u).has_value());
  RE_CHECK(!Quantity::make(static_cast<Unit>(200u), 1u).has_value());

  const auto max = Quantity::make(Unit::kCount, 18446744073709551615ull).value();
  RE_CHECK(!Quantity::checked_add(max, Quantity::make(Unit::kCount, 1u).value()).has_value());
  RE_CHECK(Quantity::checked_add(max, Quantity::make(Unit::kCount, 0u).value()).has_value());
}

RE_TEST(model, quantity_parse_is_strict) {
  RE_CHECK_EQ(Quantity::parse(Unit::kCount, "0").value().units(), std::uint64_t{0});
  RE_CHECK_EQ(Quantity::parse(Unit::kCount, "18446744073709551615").value().units(),
              18446744073709551615ull);
  RE_CHECK(!Quantity::parse(Unit::kCount, "18446744073709551616").has_value());
  RE_CHECK(!Quantity::parse(Unit::kCount, "").has_value());
  RE_CHECK(!Quantity::parse(Unit::kCount, "-1").has_value());
  RE_CHECK(!Quantity::parse(Unit::kCount, "+1").has_value());
  RE_CHECK(!Quantity::parse(Unit::kCount, " 1").has_value());
  RE_CHECK(!Quantity::parse(Unit::kCount, "1_000").has_value());
  RE_CHECK(!Quantity::parse(Unit::kCount, "a").has_value());
  RE_CHECK(!Quantity::parse(Unit::kCount, "000000000000000000000").has_value());
}

RE_TEST(model, priority_ordering_is_total_and_deterministic) {
  Priority background;
  background.klass = PriorityClass::kBackground;
  Priority standard_low;
  standard_low.klass = PriorityClass::kStandard;
  standard_low.rank = 9u;
  Priority standard_high;
  standard_high.klass = PriorityClass::kStandard;
  standard_high.rank = 0u;
  Priority guaranteed;
  guaranteed.klass = PriorityClass::kGuaranteed;

  RE_CHECK(entl::compare_priority(guaranteed, standard_high) < 0);
  RE_CHECK(entl::compare_priority(standard_high, standard_low) < 0);
  RE_CHECK(entl::compare_priority(standard_low, background) < 0);
  RE_CHECK_EQ(entl::compare_priority(standard_low, standard_low), 0);
  RE_CHECK(entl::compare_priority(background, guaranteed) > 0);
}

RE_TEST(model, fence_mask_requires_the_control_epoch) {
  RE_CHECK(entl::FenceMask::standard().is_valid());
  RE_CHECK(entl::FenceMask::all().is_valid());
  RE_CHECK(entl::FenceMask::control_epoch_only().is_valid());
  RE_CHECK(!entl::FenceMask::of(0u).is_valid());
  RE_CHECK(!entl::FenceMask::of(0x0080u).is_valid());
  RE_CHECK(entl::FenceMask::of(0x0011u).is_valid());
  RE_CHECK(!entl::FenceMask::of(0x0090u).is_valid());
  RE_CHECK(entl::FenceMask::all().has(entl::FenceBit::kEnvelopeBindingDigest));
  RE_CHECK(!entl::FenceMask::standard().has(entl::FenceBit::kPolicyContextDigest));

  entl::CanonicalWriter writer;
  entl::FenceMask::all().encode(writer);
  entl::CanonicalReader reader(writer.data());
  RE_CHECK_EQ(entl::FenceMask::decode(reader).bits(), entl::FenceMask::all().bits());
}

RE_TEST(model, snapshot_and_binding_digests_are_sensitive) {
  const entl::AuthoritySnapshot snapshot = matching_snapshot();
  const entl::AuthoritySnapshot copy = matching_snapshot();
  RE_CHECK_EQ(snapshot.snapshot_digest().to_hex(), copy.snapshot_digest().to_hex());

  entl::AuthoritySnapshot changed = snapshot;
  changed.facility_capacity_generation = entl::Generation::from_value(6u);
  RE_CHECK_NE(changed.snapshot_digest().to_hex(), snapshot.snapshot_digest().to_hex());

  const entl::AuthorityBinding binding = binding_for(snapshot);
  RE_CHECK_EQ(binding.binding_digest().to_hex(), binding_for(snapshot).binding_digest().to_hex());
  entl::AuthorityBinding changed_binding = binding;
  changed_binding.admission_decision_digest = re_test::digest_of("other");
  RE_CHECK_NE(changed_binding.binding_digest().to_hex(), binding.binding_digest().to_hex());
}

RE_TEST(model, fence_evaluation_reports_every_fault_in_order) {
  const entl::AuthoritySnapshot snapshot = matching_snapshot();
  const entl::AuthorityBinding binding = binding_for(snapshot);
  auto clean = entl::evaluate_fence(binding, snapshot, entl::FenceMask::all());
  RE_CHECK(!clean.stale);
  RE_CHECK_EQ(clean.primary, ErrorCode::kOk);

  entl::AuthoritySnapshot changed = snapshot;
  changed.control_epoch = entl::Epoch::from_value(10u);
  changed.facility_capacity_generation = entl::Generation::from_value(6u);
  changed.policy_context_digest = re_test::digest_of("different policy");
  auto stale = entl::evaluate_fence(binding, changed, entl::FenceMask::all());
  RE_CHECK(stale.stale);
  RE_CHECK_EQ(stale.primary, ErrorCode::kStaleEpoch);
  RE_REQUIRE(stale.faults.size() >= 3u);
  RE_CHECK_EQ(stale.faults[1], ErrorCode::kStaleBinding);
  RE_CHECK(stale.epoch_mismatch());

  // The control epoch is fenced even when every other bit is masked out.
  auto epoch_only = entl::evaluate_fence(binding, changed, entl::FenceMask::control_epoch_only());
  RE_CHECK(epoch_only.stale);
  RE_CHECK_EQ(epoch_only.faults.size(), std::size_t{1});
}

RE_TEST(model, liveness_precedence_is_deterministic) {
  const entl::AuthoritySnapshot snapshot = matching_snapshot();
  const Entitlement record = make_record();
  const Timestamp inside = re_test::instant(1700000000);

  RE_CHECK(entl::evaluate_liveness(record, snapshot, inside).live);

  // Exactly at expires_at the entitlement is already expired.
  RE_CHECK_EQ(entl::evaluate_liveness(record, snapshot, record.expires_at).primary, ErrorCode::kExpired);
  auto just_before = record.expires_at.checked_add_nanos(-1);
  RE_REQUIRE(just_before.has_value());
  RE_CHECK(entl::evaluate_liveness(record, snapshot, just_before.value()).live);
  RE_CHECK_EQ(entl::evaluate_liveness(record, snapshot, record.effective_from).primary, ErrorCode::kOk);
  auto before = record.effective_from.checked_add_nanos(-1);
  RE_REQUIRE(before.has_value());
  RE_CHECK_EQ(entl::evaluate_liveness(record, snapshot, before.value()).primary, ErrorCode::kNotYetEffective);

  Entitlement revoked = record;
  revoked.state = EntitlementState::kRevoked;
  auto revoked_view = entl::evaluate_liveness(revoked, snapshot, inside);
  RE_CHECK_EQ(revoked_view.primary, ErrorCode::kRevoked);
  RE_CHECK(revoked_view.live == false);

  Entitlement suspended = record;
  suspended.state = EntitlementState::kSuspended;
  RE_CHECK_EQ(entl::evaluate_liveness(suspended, snapshot, inside).primary, ErrorCode::kSuspended);

  Entitlement superseded = record;
  superseded.state = EntitlementState::kSuperseded;
  RE_CHECK_EQ(entl::evaluate_liveness(superseded, snapshot, inside).primary, ErrorCode::kSuperseded);

  Entitlement exhausted = record;
  exhausted.remaining = Quantity::make(Unit::kCount, 0u).value();
  RE_CHECK_EQ(entl::evaluate_liveness(exhausted, snapshot, inside).primary, ErrorCode::kQuantityExceeded);

  // A stale epoch outranks a revoked state, but the revocation is still reported.
  entl::AuthoritySnapshot newer = snapshot;
  newer.control_epoch = entl::Epoch::from_value(10u);
  auto stale_revoked = entl::evaluate_liveness(revoked, newer, inside);
  RE_CHECK_EQ(stale_revoked.primary, ErrorCode::kStaleEpoch);
  RE_REQUIRE(stale_revoked.faults.size() >= 2u);
  RE_CHECK_EQ(stale_revoked.faults[1], ErrorCode::kRevoked);
  RE_CHECK(stale_revoked.stale);
}

RE_TEST(model, entitlement_codec_round_trips_exactly) {
  Entitlement record = make_record();
  record.note = "a note with accents \xc3\xa9";
  record.suspended_at = re_test::instant(1700000001);
  record.terminal_reason = entl::TerminalReason::kNotApplicable;
  entl::HolderChange change;
  change.from = record.holder;
  change.to = re_test::must_parse<entl::TenantId>("tenant-b");
  change.at = re_test::instant(1700000002);
  change.commit_seq = entl::Sequence::from_value(9u);
  change.actor = record.created_by;
  record.holder_history.push_back(change);

  entl::CanonicalWriter writer;
  record.encode(writer);
  entl::CanonicalReader reader(writer.data());
  const Entitlement decoded = Entitlement::decode(reader);
  RE_REQUIRE(reader.ok());
  RE_CHECK(reader.finish().has_value());

  entl::CanonicalWriter again;
  decoded.encode(again);
  RE_CHECK(writer.data() == again.data());
  RE_CHECK_EQ(decoded.note, record.note);
  RE_CHECK_EQ(decoded.holder_history.size(), std::size_t{1});
  RE_CHECK_EQ(decoded.binding.binding_digest().to_hex(), record.binding.binding_digest().to_hex());
}

RE_TEST(model, entitlement_codec_rejects_invariant_violations) {
  const Entitlement good = make_record();

  Entitlement too_much = good;
  too_much.remaining = Quantity::make(Unit::kCount, 1000u).value();
  entl::CanonicalWriter writer;
  too_much.encode(writer);
  entl::CanonicalReader reader(writer.data());
  (void)Entitlement::decode(reader);
  RE_CHECK(!reader.ok());

  Entitlement bad_window = good;
  bad_window.expires_at = bad_window.effective_from;
  entl::CanonicalWriter window_writer;
  bad_window.encode(window_writer);
  entl::CanonicalReader window_reader(window_writer.data());
  (void)Entitlement::decode(window_reader);
  RE_CHECK(!window_reader.ok());

  Entitlement bad_fence = good;
  bad_fence.fence = entl::FenceMask::of(0u);
  entl::CanonicalWriter fence_writer;
  bad_fence.encode(fence_writer);
  entl::CanonicalReader fence_reader(fence_writer.data());
  (void)Entitlement::decode(fence_reader);
  RE_CHECK(!fence_reader.ok());

  Entitlement bad_unit = good;
  bad_unit.unit = Unit::kGibibytes;
  entl::CanonicalWriter unit_writer;
  bad_unit.encode(unit_writer);
  entl::CanonicalReader unit_reader(unit_writer.data());
  (void)Entitlement::decode(unit_reader);
  RE_CHECK(!unit_reader.ok());

  Entitlement bad_root = good;
  bad_root.root = entl::EntitlementId::derive("other root");
  entl::CanonicalWriter root_writer;
  bad_root.encode(root_writer);
  entl::CanonicalReader root_reader(root_writer.data());
  (void)Entitlement::decode(root_reader);
  RE_CHECK(!root_reader.ok());
}

RE_TEST(model, mutation_outcome_codec_round_trips) {
  entl::MutationOutcome outcome;
  outcome.code = ErrorCode::kOk;
  outcome.request_id = re_test::request_id("outcome");
  outcome.request_digest = re_test::digest_of("digest");
  outcome.commit_seq = entl::Sequence::from_value(11u);
  outcome.committed_at = re_test::instant(1700000123);
  outcome.primary_revision = entl::Revision::from_value(4u);
  outcome.primary_id = entl::EntitlementId::derive("entitlement");
  outcome.affected = {outcome.primary_id, entl::EntitlementId::derive("second")};
  outcome.message = "committed";

  entl::CanonicalWriter writer;
  outcome.encode(writer);
  entl::CanonicalReader reader(writer.data());
  const entl::MutationOutcome decoded = entl::MutationOutcome::decode(reader);
  RE_REQUIRE(reader.ok());
  RE_CHECK(reader.finish().has_value());
  RE_CHECK_EQ(decoded.request_id.to_hex(), outcome.request_id.to_hex());
  RE_CHECK_EQ(decoded.commit_seq.value(), outcome.commit_seq.value());
  RE_CHECK_EQ(decoded.affected.size(), std::size_t{2});
  RE_CHECK_EQ(decoded.message, outcome.message);
  RE_CHECK(!decoded.replayed);
}

RE_TEST(model, token_round_trip_and_tamper_detection) {
  const Entitlement record = make_record();
  const entl::EntitlementToken token = entl::EntitlementToken::issue(record);
  const std::vector<std::uint8_t> bytes = token.encode();
  auto decoded = entl::EntitlementToken::decode(bytes);
  RE_REQUIRE(decoded.has_value());
  RE_CHECK_EQ(decoded->id().to_hex(), record.id.to_hex());
  RE_CHECK_EQ(decoded->token_digest().to_hex(), token.token_digest().to_hex());

  for (std::size_t index = 0; index < bytes.size(); ++index) {
    std::vector<std::uint8_t> tampered = bytes;
    tampered[index] = static_cast<std::uint8_t>(tampered[index] ^ 0x5Au);
    auto result = entl::EntitlementToken::decode(tampered);
    RE_CHECK(!result.has_value());
  }
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    auto result = entl::EntitlementToken::decode(std::span<const std::uint8_t>(bytes.data(), length));
    RE_CHECK(!result.has_value());
  }
}

RE_TEST(model, transfer_mode_and_state_names_are_stable) {
  RE_CHECK_EQ(std::string(entl::transfer_mode_name(entl::TransferMode::kMove)), std::string("move"));
  RE_CHECK_EQ(std::string(entl::transfer_mode_name(entl::TransferMode::kSplit)), std::string("split"));
  RE_CHECK_EQ(std::string(entl::transfer_mode_name(entl::TransferMode::kDelegate)), std::string("delegate"));
  RE_CHECK(entl::transfer_mode_from_name("split").has_value());
  RE_CHECK(!entl::transfer_mode_from_name("teleport").has_value());
  RE_CHECK_EQ(std::string(entl::entitlement_state_name(EntitlementState::kSuperseded)),
              std::string("superseded"));
  RE_CHECK(entl::is_terminal_state(EntitlementState::kRevoked));
  RE_CHECK(!entl::is_terminal_state(EntitlementState::kSuspended));
  RE_CHECK_EQ(std::string(entl::error_code_name(ErrorCode::kStaleEpoch)), std::string("stale_epoch"));
  RE_CHECK(entl::error_code_is_validation(ErrorCode::kInvalidIdentifier));
  RE_CHECK(!entl::error_code_is_validation(ErrorCode::kRevoked));
  RE_CHECK(entl::error_code_is_durability(ErrorCode::kRollbackDetected));
  RE_CHECK(entl::error_code_is_authority_refusal(ErrorCode::kStaleBinding));
}

RE_TEST(model, scope_matching_requires_every_component) {
  entl::ResourceScope first;
  first.facility = re_test::must_parse<entl::FacilityId>("dc-1");
  first.resource_type = re_test::must_parse<entl::ResourceTypeId>("accelerator");
  first.scope = re_test::must_parse<entl::ResourceScopeId>("pool-a");
  entl::ResourceScope second = first;
  RE_CHECK(entl::scope_matches(first, second));
  second.scope = re_test::must_parse<entl::ResourceScopeId>("pool-b");
  RE_CHECK(!entl::scope_matches(first, second));
  RE_CHECK(first.is_set());
  RE_CHECK(!entl::ResourceScope{}.is_set());
}
