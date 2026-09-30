// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <string>
#include <vector>

#include "harness.hpp"
#include "resource_entitlement/store.hpp"
#include "test_support.hpp"

using entl::ErrorCode;
using entl::EntitlementState;
using entl::MutationOutcome;
using entl::Unit;
using entl::VerificationDecision;
using re_test::StoreFixture;

namespace {

entl::VerifyRequest verify_at(const entl::EntitlementId& id, entl::Timestamp now) {
  entl::VerifyRequest request;
  request.id = id;
  request.now = now;
  return request;
}

}  // namespace

RE_TEST(lifecycle, fresh_store_bootstraps_a_published_authority) {
  StoreFixture fixture;
  const entl::AuthoritySnapshot authority = fixture.store().authority();
  RE_CHECK_EQ(authority.revision.value(), std::uint64_t{1});
  RE_CHECK_EQ(authority.control_epoch.value(), std::uint64_t{1});
  RE_CHECK_EQ(fixture.store().commit_seq().value(), std::uint64_t{0});
  RE_CHECK_EQ(fixture.store().stats().entitlement_count, std::size_t{0});
  RE_CHECK(!fixture.store().is_read_only());
  RE_CHECK(fixture.store().directory().filename() == fixture.path().filename());
}

RE_TEST(lifecycle, verification_of_an_unknown_identity_is_a_decision_not_an_error) {
  StoreFixture fixture;
  auto decision = fixture.store().verify(verify_at(entl::EntitlementId::derive("missing"),
                                                   re_test::instant(1700000000)));
  RE_REQUIRE(decision.has_value());
  RE_CHECK(!decision->authorized);
  RE_CHECK_EQ(decision->primary, ErrorCode::kUnknownEntitlement);
  RE_CHECK(!decision->explanation.empty());
}

RE_TEST(lifecycle, authority_update_requires_the_expected_snapshot_revision) {
  StoreFixture fixture;
  const std::uint64_t capacity = 1;
  (void)fixture.publish_authority(capacity);
  const entl::Revision current = fixture.store().authority().revision;

  entl::AuthorityUpdateRequest stale;
  stale.request_id = re_test::request_id("stale-authority");
  stale.now = fixture.now();
  stale.expected_snapshot_revision = entl::Revision::from_value(1u);
  stale.facility_capacity_generation = entl::Generation::from_value(2u);
  stale.publisher = re_test::must_parse<entl::ActorId>("facility-capacity");
  RE_CHECK_ERR(fixture.store().update_authority(stale), ErrorCode::kRevisionConflict);

  entl::AuthorityUpdateRequest good;
  good.request_id = re_test::request_id("good-authority");
  good.now = fixture.now();
  good.expected_snapshot_revision = current;
  good.facility_capacity_generation = entl::Generation::from_value(2u);
  good.publisher = re_test::must_parse<entl::ActorId>("facility-capacity");
  RE_REQUIRE_OK(fixture.store().update_authority(good));
  RE_CHECK_EQ(fixture.store().authority().facility_capacity_generation.value(), std::uint64_t{2});
  RE_CHECK_EQ(fixture.store().authority().revision.value(), current.value() + 1u);
}

RE_TEST(lifecycle, authority_update_can_advance_the_control_epoch) {
  StoreFixture fixture;
  const entl::Revision revision = fixture.publish_authority(1u);
  entl::AuthorityUpdateRequest request;
  request.request_id = re_test::request_id("epoch-advance");
  request.now = fixture.now();
  request.expected_snapshot_revision = revision;
  request.advance_epoch = true;
  request.publisher = re_test::must_parse<entl::ActorId>("control-plane");
  RE_REQUIRE_OK(fixture.store().update_authority(request));
  RE_CHECK_EQ(fixture.store().authority().control_epoch.value(), std::uint64_t{2});
}

RE_TEST(lifecycle, grant_produces_live_authority_with_full_provenance) {
  StoreFixture fixture;
  const entl::Revision revision = fixture.publish_authority(1u);
  const entl::EntitlementId id = fixture.grant_default("grant-1", 100u);

  auto record = fixture.store().get(id);
  RE_REQUIRE(record.has_value());
  RE_CHECK_EQ(record->state, EntitlementState::kActive);
  RE_CHECK_EQ(record->remaining.units(), std::uint64_t{100});
  RE_CHECK_EQ(record->granted.units(), std::uint64_t{100});
  RE_CHECK_EQ(record->binding.authority_revision.value(), revision.value());
  RE_CHECK(record->has_admission_evidence());
  RE_CHECK_EQ(record->relation, entl::LineageRelation::kRoot);
  RE_CHECK(record->root == record->id);
  RE_CHECK_EQ(record->holder.str(), std::string("tenant-a"));

  auto decision = fixture.store().verify(verify_at(id, fixture.now()));
  RE_REQUIRE(decision.has_value());
  RE_CHECK(decision->authorized);
  RE_CHECK_EQ(decision->primary, ErrorCode::kOk);
  RE_CHECK_EQ(decision->observed_commit_seq.value(), fixture.store().commit_seq().value());

  entl::ListFilter filter;
  auto listed = fixture.store().list(filter);
  RE_REQUIRE(listed.has_value());
  RE_CHECK_EQ(listed->size(), std::size_t{1});
  RE_CHECK_EQ(fixture.store().stats().entitlement_count, std::size_t{1});
  RE_CHECK_EQ(fixture.store().stats().live_count, std::size_t{1});
}

RE_TEST(lifecycle, grant_refuses_missing_and_malformed_inputs) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);

  entl::GrantRequest request = fixture.make_grant("bad-grant", 10u);
  request.admission_decision_digest = entl::Sha256Digest{};
  RE_CHECK_ERR(fixture.store().grant(request), ErrorCode::kMissingAdmissionEvidence);

  request = fixture.make_grant("bad-grant", 10u);
  request.quantity = entl::Quantity::make(Unit::kCount, 0u).value();
  RE_CHECK_ERR(fixture.store().grant(request), ErrorCode::kInvalidQuantity);

  request = fixture.make_grant("bad-grant", 10u);
  request.expires_at = request.effective_from;
  RE_CHECK_ERR(fixture.store().grant(request), ErrorCode::kWindowViolation);

  request = fixture.make_grant("bad-grant", 10u);
  request.effective_from = entl::Timestamp{};
  RE_CHECK_ERR(fixture.store().grant(request), ErrorCode::kInvalidTimestamp);

  request = fixture.make_grant("bad-grant", 10u);
  request.now = entl::Timestamp{};
  RE_CHECK_ERR(fixture.store().grant(request), ErrorCode::kInvalidTimestamp);

  request = fixture.make_grant("bad-grant", 10u);
  request.holder = entl::TenantId{};
  RE_CHECK_ERR(fixture.store().grant(request), ErrorCode::kMissingField);

  request = fixture.make_grant("bad-grant", 10u);
  request.actor = entl::ActorId{};
  RE_CHECK_ERR(fixture.store().grant(request), ErrorCode::kMissingField);

  request = fixture.make_grant("bad-grant", 10u);
  request.fence = entl::FenceMask::of(0u);
  RE_CHECK_ERR(fixture.store().grant(request), ErrorCode::kInvalidFenceMask);

  request = fixture.make_grant("bad-grant", 10u);
  request.unit = Unit::kGibibytes;
  RE_CHECK_ERR(fixture.store().grant(request), ErrorCode::kUnitMismatch);

  request = fixture.make_grant("bad-grant", 10u);
  request.priority.klass = static_cast<entl::PriorityClass>(99u);
  RE_CHECK_ERR(fixture.store().grant(request), ErrorCode::kInvalidPriority);

  RE_CHECK_EQ(fixture.store().stats().entitlement_count, std::size_t{0});
  RE_CHECK_EQ(fixture.store().commit_seq().value(), std::uint64_t{1});
}

RE_TEST(lifecycle, grant_replay_is_idempotent_and_does_not_mint_twice) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const entl::GrantRequest request = fixture.make_grant("replayed-grant", 50u);
  auto first = fixture.store().grant(request);
  RE_REQUIRE(first.has_value());
  RE_CHECK(!first->replayed);
  const std::uint64_t commits_after_first = fixture.store().commit_seq().value();

  auto replay = fixture.store().grant(request);
  RE_REQUIRE(replay.has_value());
  RE_CHECK(replay->replayed);
  RE_CHECK_EQ(replay->primary_id.to_hex(), first->primary_id.to_hex());
  RE_CHECK_EQ(replay->commit_seq.value(), first->commit_seq.value());
  RE_CHECK_EQ(fixture.store().commit_seq().value(), commits_after_first);
  RE_CHECK_EQ(fixture.store().stats().entitlement_count, std::size_t{1});
  RE_CHECK_EQ(fixture.store().stats().replays, std::uint64_t{1});

  entl::GrantRequest conflicting = request;
  conflicting.quantity = entl::Quantity::make(Unit::kCount, 51u).value();
  RE_CHECK_ERR(fixture.store().grant(conflicting), ErrorCode::kRequestIdConflict);

  auto lookup = fixture.store().lookup_outcome(request.request_id);
  RE_REQUIRE(lookup.has_value());
  RE_CHECK(lookup->replayed);
  RE_CHECK_EQ(lookup->primary_id.to_hex(), first->primary_id.to_hex());
  RE_CHECK_ERR(fixture.store().lookup_outcome(re_test::request_id("never-seen")),
               ErrorCode::kNoRecordedOutcome);
}

RE_TEST(lifecycle, mutation_preconditions_reject_stale_revisions) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const entl::EntitlementId id = fixture.grant_default("revision-guard", 40u);
  auto record = fixture.store().get(id);
  RE_REQUIRE(record.has_value());
  const entl::Revision current = record->revision;

  entl::SuspendRequest suspend;
  suspend.request_id = re_test::request_id("suspend-1");
  suspend.now = fixture.now();
  suspend.id = id;
  suspend.expected_revision = current;
  suspend.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().suspend(suspend));

  entl::SuspendRequest again = suspend;
  again.request_id = re_test::request_id("suspend-2");
  RE_CHECK_ERR(fixture.store().suspend(again), ErrorCode::kRevisionConflict);

  entl::RevokeRequest stale_revoke;
  stale_revoke.request_id = re_test::request_id("revoke-stale");
  stale_revoke.now = fixture.now();
  stale_revoke.id = id;
  stale_revoke.expected_revision = current;
  stale_revoke.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_CHECK_ERR(fixture.store().revoke(stale_revoke), ErrorCode::kRevisionConflict);
}

RE_TEST(lifecycle, suspend_and_resume_control_liveness) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const entl::EntitlementId id = fixture.grant_default("suspend-target", 30u);

  entl::SuspendRequest suspend;
  suspend.request_id = re_test::request_id("suspend-target-op");
  suspend.now = fixture.now();
  suspend.id = id;
  suspend.expected_revision = fixture.store().get(id).value().revision;
  suspend.actor = re_test::must_parse<entl::ActorId>("operator-1");
  suspend.note = "planned maintenance";
  RE_REQUIRE_OK(fixture.store().suspend(suspend));

  auto suspended = fixture.store().get(id);
  RE_REQUIRE(suspended.has_value());
  RE_CHECK_EQ(suspended->state, EntitlementState::kSuspended);
  RE_CHECK(suspended->suspended_at.has_value());
  RE_CHECK_EQ(suspended->note, std::string("planned maintenance"));

  auto decision = fixture.store().verify(verify_at(id, fixture.now()));
  RE_REQUIRE(decision.has_value());
  RE_CHECK(!decision->authorized);
  RE_CHECK_EQ(decision->primary, ErrorCode::kSuspended);

  entl::DrawRequest draw;
  draw.request_id = re_test::request_id("draw-while-suspended");
  draw.now = fixture.now();
  draw.id = id;
  draw.expected_revision = suspended->revision;
  draw.quantity = entl::Quantity::make(Unit::kCount, 1u).value();
  draw.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_CHECK_ERR(fixture.store().draw(draw), ErrorCode::kSuspended);

  entl::ResumeRequest resume;
  resume.request_id = re_test::request_id("resume-target");
  resume.now = fixture.now();
  resume.id = id;
  resume.expected_revision = suspended->revision;
  resume.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().resume(resume));

  auto resumed = fixture.store().get(id);
  RE_REQUIRE(resumed.has_value());
  RE_CHECK_EQ(resumed->state, EntitlementState::kActive);
  RE_CHECK(!resumed->suspended_at.has_value());
  RE_CHECK(fixture.store().verify(verify_at(id, fixture.now()))->authorized);
}

RE_TEST(lifecycle, revocation_is_immediate_and_terminal) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const entl::EntitlementId id = fixture.grant_default("revoke-target", 30u);
  const entl::Revision before = fixture.store().get(id).value().revision;

  entl::RevokeRequest revoke;
  revoke.request_id = re_test::request_id("revoke-target-op");
  revoke.now = fixture.now();
  revoke.id = id;
  revoke.expected_revision = before;
  revoke.reason = entl::TerminalReason::kPolicyRevocation;
  revoke.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().revoke(revoke));

  auto record = fixture.store().get(id);
  RE_REQUIRE(record.has_value());
  RE_CHECK_EQ(record->state, EntitlementState::kRevoked);
  RE_CHECK_EQ(record->terminal_reason, entl::TerminalReason::kPolicyRevocation);
  RE_CHECK(record->revoked_at.has_value());
  RE_CHECK_EQ(record->remaining.units(), std::uint64_t{30});

  auto decision = fixture.store().verify(verify_at(id, fixture.now()));
  RE_REQUIRE(decision.has_value());
  RE_CHECK(!decision->authorized);
  RE_CHECK_EQ(decision->primary, ErrorCode::kRevoked);

  entl::RevokeRequest again = revoke;
  again.request_id = re_test::request_id("revoke-target-2");
  again.expected_revision = record->revision;
  RE_CHECK_ERR(fixture.store().revoke(again), ErrorCode::kInvalidTransition);

  entl::SuspendRequest suspend;
  suspend.request_id = re_test::request_id("suspend-revoked");
  suspend.now = fixture.now();
  suspend.id = id;
  suspend.expected_revision = record->revision;
  suspend.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_CHECK_ERR(fixture.store().suspend(suspend), ErrorCode::kInvalidTransition);

  entl::RevokeRequest bad_reason = revoke;
  bad_reason.request_id = re_test::request_id("revoke-bad-reason");
  bad_reason.reason = entl::TerminalReason::kExpiredByTime;
  RE_CHECK_ERR(fixture.store().revoke(bad_reason), ErrorCode::kInvalidArgument);
}

RE_TEST(lifecycle, expiry_boundary_is_exact_and_sweep_is_accounting_only) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const entl::EntitlementId id = fixture.grant_default("expiry-target", 20u);
  const entl::Entitlement expiry = fixture.store().get(id).value();

  auto before = expiry.expires_at.checked_add_nanos(-1);
  RE_REQUIRE(before.has_value());
  RE_CHECK(fixture.store().verify(verify_at(id, before.value()))->authorized);

  auto decision = fixture.store().verify(verify_at(id, expiry.expires_at));
  RE_REQUIRE(decision.has_value());
  RE_CHECK(!decision->authorized);
  RE_CHECK_EQ(decision->primary, ErrorCode::kExpired);

  // The record is still kActive: expiry is derived from authoritative time and
  // must not require the sweep to have run.
  RE_CHECK_EQ(fixture.store().get(id).value().state, EntitlementState::kActive);

  entl::ExpireRequest early;
  early.request_id = re_test::request_id("expire-early");
  early.now = before.value();
  early.id = id;
  early.expected_revision = expiry.revision;
  early.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_CHECK_ERR(fixture.store().expire(early), ErrorCode::kInvalidTransition);

  entl::ExpireRequest sweep = early;
  sweep.request_id = re_test::request_id("expire-sweep");
  sweep.now = expiry.expires_at;
  RE_REQUIRE_OK(fixture.store().expire(sweep));
  RE_CHECK_EQ(fixture.store().get(id).value().state, EntitlementState::kExpired);
  RE_CHECK_EQ(fixture.store().get(id).value().terminal_reason, entl::TerminalReason::kExpiredByTime);

  entl::ExpireRequest twice = sweep;
  twice.request_id = re_test::request_id("expire-twice");
  twice.expected_revision = fixture.store().get(id).value().revision;
  RE_CHECK_ERR(fixture.store().expire(twice), ErrorCode::kInvalidTransition);
}

RE_TEST(lifecycle, draw_and_release_account_correctly) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const entl::EntitlementId id = fixture.grant_default("accounting", 10u);

  entl::DrawRequest draw;
  draw.request_id = re_test::request_id("draw-1");
  draw.now = fixture.now();
  draw.id = id;
  draw.expected_revision = fixture.store().get(id).value().revision;
  draw.quantity = entl::Quantity::make(Unit::kCount, 4u).value();
  draw.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().draw(draw));
  RE_CHECK_EQ(fixture.store().get(id).value().remaining.units(), std::uint64_t{6});
  RE_CHECK_EQ(fixture.store().get(id).value().granted.units(), std::uint64_t{10});

  entl::DrawRequest too_much = draw;
  too_much.request_id = re_test::request_id("draw-2");
  too_much.expected_revision = fixture.store().get(id).value().revision;
  too_much.quantity = entl::Quantity::make(Unit::kCount, 7u).value();
  RE_CHECK_ERR(fixture.store().draw(too_much), ErrorCode::kQuantityExceeded);

  entl::DrawRequest wrong_unit = draw;
  wrong_unit.request_id = re_test::request_id("draw-3");
  wrong_unit.expected_revision = fixture.store().get(id).value().revision;
  wrong_unit.quantity = entl::Quantity::make(Unit::kGibibytes, 1u).value();
  RE_CHECK_ERR(fixture.store().draw(wrong_unit), ErrorCode::kUnitMismatch);

  entl::ReleaseRequest release;
  release.request_id = re_test::request_id("release-1");
  release.now = fixture.now();
  release.id = id;
  release.expected_revision = fixture.store().get(id).value().revision;
  release.quantity = entl::Quantity::make(Unit::kCount, 2u).value();
  release.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().release(release));
  RE_CHECK_EQ(fixture.store().get(id).value().remaining.units(), std::uint64_t{8});

  entl::ReleaseRequest over_release = release;
  over_release.request_id = re_test::request_id("release-2");
  over_release.expected_revision = fixture.store().get(id).value().revision;
  over_release.quantity = entl::Quantity::make(Unit::kCount, 3u).value();
  RE_CHECK_ERR(fixture.store().release(over_release), ErrorCode::kLimitExceeded);

  entl::DrawRequest drain = draw;
  drain.request_id = re_test::request_id("draw-drain");
  drain.expected_revision = fixture.store().get(id).value().revision;
  drain.quantity = entl::Quantity::make(Unit::kCount, 8u).value();
  RE_REQUIRE_OK(fixture.store().draw(drain));
  RE_CHECK(fixture.store().get(id).value().is_exhausted());

  auto decision = fixture.store().verify(verify_at(id, fixture.now()));
  RE_REQUIRE(decision.has_value());
  RE_CHECK(!decision->authorized);
  RE_CHECK_EQ(decision->primary, ErrorCode::kQuantityExceeded);
}

RE_TEST(lifecycle, verification_reports_expected_context_mismatches) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const entl::EntitlementId id = fixture.grant_default("context-check", 5u);

  entl::VerifyRequest request = verify_at(id, fixture.now());
  entl::ExpectedContext context;
  context.holder = re_test::must_parse<entl::TenantId>("tenant-b");
  context.service = re_test::must_parse<entl::ServiceId>("inference");
  context.service_class = re_test::must_parse<entl::ServiceClassId>("gold");
  context.scope.facility = re_test::must_parse<entl::FacilityId>("dc-1");
  context.scope.resource_type = re_test::must_parse<entl::ResourceTypeId>("accelerator");
  context.scope.scope = re_test::must_parse<entl::ResourceScopeId>("scope-a");
  context.unit = Unit::kCount;
  request.expected = context;
  auto decision = fixture.store().verify(request);
  RE_REQUIRE(decision.has_value());
  RE_CHECK(!decision->authorized);
  RE_CHECK_EQ(decision->primary, ErrorCode::kTenantMismatch);

  request.expected->holder = re_test::must_parse<entl::TenantId>("tenant-a");
  request.expected->scope.scope = re_test::must_parse<entl::ResourceScopeId>("scope-z");
  request.requested = entl::Quantity::make(Unit::kCount, 9u).value();
  decision = fixture.store().verify(request);
  RE_REQUIRE(decision.has_value());
  RE_CHECK_EQ(decision->primary, ErrorCode::kScopeMismatch);
  RE_REQUIRE(decision->secondary_faults.size() >= 1u);
  RE_CHECK_EQ(decision->secondary_faults[0], ErrorCode::kQuantityExceeded);

  request.expected.reset();
  request.requested = entl::Quantity::make(Unit::kCount, 5u).value();
  decision = fixture.store().verify(request);
  RE_REQUIRE(decision.has_value());
  RE_CHECK(decision->authorized);

  request.requested = entl::Quantity::make(Unit::kCount, 6u).value();
  decision = fixture.store().verify(request);
  RE_REQUIRE(decision.has_value());
  RE_CHECK_EQ(decision->primary, ErrorCode::kQuantityExceeded);
}

RE_TEST(lifecycle, commits_emit_events_after_the_durable_point) {
  std::vector<entl::Event> events;
  entl::StoreOpenOptions options;
  options.create_if_missing = true;
  options.event_sink = [&events](const entl::Event& event) { events.push_back(event); };
  StoreFixture fixture(options);
  (void)fixture.publish_authority(1u);
  const entl::EntitlementId id = fixture.grant_default("event-target", 3u);

  RE_REQUIRE(!events.empty());
  bool saw_grant = false;
  bool saw_open = false;
  for (const entl::Event& event : events) {
    if (event.kind == entl::EventKind::kStoreOpened) {
      saw_open = true;
    }
    if (event.kind == entl::EventKind::kGranted) {
      saw_grant = true;
      RE_CHECK_EQ(event.id.to_hex(), id.to_hex());
      RE_CHECK(event.commit_seq.value() > 0u);
      RE_CHECK_EQ(event.code, ErrorCode::kOk);
    }
  }
  RE_CHECK(saw_open);
  RE_CHECK(saw_grant);
}

RE_TEST(lifecycle, read_only_store_refuses_mutations_and_serves_reads) {
  re_test::TempDirectory holder("readonly");
  entl::EntitlementId target;
  entl::Revision target_revision;
  {
    auto opened = entl::Store::open(re_test::test_options(holder.path()));
    if (!opened.has_value()) {
      RE_FAIL("writable open failed: " + opened.error().to_string());
    }
    RE_REQUIRE(opened.has_value());
    entl::Store& store = *opened.value();
    RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
        "ro-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 3u)));

    entl::GrantRequest grant;
    grant.request_id = re_test::request_id("ro-grant");
    grant.now = re_test::instant(1700000001);
    grant.holder = re_test::must_parse<entl::TenantId>("tenant-a");
    grant.service = re_test::must_parse<entl::ServiceId>("inference");
    grant.service_class = re_test::must_parse<entl::ServiceClassId>("gold");
    grant.scope.facility = re_test::must_parse<entl::FacilityId>("dc-1");
    grant.scope.resource_type = re_test::must_parse<entl::ResourceTypeId>("accelerator");
    grant.scope.scope = re_test::must_parse<entl::ResourceScopeId>("scope-a");
    grant.unit = Unit::kCount;
    grant.quantity = entl::Quantity::make(Unit::kCount, 12u).value();
    grant.effective_from = re_test::instant(1699999000);
    grant.expires_at = re_test::instant(1799999000);
    grant.admission_decision_digest = re_test::digest_of("admission");
    grant.fence = entl::FenceMask::standard();
    grant.actor = re_test::must_parse<entl::ActorId>("operator-1");
    auto outcome = store.grant(grant);
    RE_REQUIRE(outcome.has_value());
    target = outcome->primary_id;
    target_revision = outcome->primary_revision;
  }

  entl::StoreOpenOptions read_only = re_test::test_options(holder.path(), false);
  read_only.read_only = true;
  auto opened = entl::Store::open(read_only);
  if (!opened.has_value()) {
    RE_FAIL("read-only open failed: " + opened.error().to_string());
  }
  RE_REQUIRE(opened.has_value());
  entl::Store& store = *opened.value();
  RE_CHECK(store.is_read_only());

  auto listed = store.list(entl::ListFilter{});
  RE_REQUIRE(listed.has_value());
  RE_CHECK_EQ(listed->size(), std::size_t{1});
  RE_CHECK_EQ(listed->front().id.to_hex(), target.to_hex());
  RE_CHECK(store.verify(verify_at(target, re_test::instant(1700000002)))->authorized);
  auto token = store.issue_token(target);
  RE_REQUIRE(token.has_value());
  RE_CHECK_EQ(token->id().to_hex(), target.to_hex());

  entl::RevokeRequest revoke;
  revoke.request_id = re_test::request_id("ro-revoke");
  revoke.now = re_test::instant(1700000002);
  revoke.id = target;
  revoke.expected_revision = target_revision;
  revoke.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_CHECK_ERR(store.revoke(revoke), ErrorCode::kReadOnlyStore);
  RE_CHECK_ERR(store.compact(), ErrorCode::kReadOnlyStore);
  RE_CHECK_ERR(store.grant(entl::GrantRequest{}), ErrorCode::kReadOnlyStore);
}
