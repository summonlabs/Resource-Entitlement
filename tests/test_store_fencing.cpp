// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <memory>
#include <string>

#include "harness.hpp"
#include "resource_entitlement/store.hpp"
#include "test_support.hpp"

using entl::ErrorCode;
using entl::EntitlementId;
using entl::Revision;
using entl::Unit;

namespace {

entl::VerifyRequest plain_verify(const EntitlementId& id, entl::Timestamp now) {
  entl::VerifyRequest request;
  request.id = id;
  request.now = now;
  return request;
}

}  // namespace

RE_TEST(fencing, a_new_capacity_generation_stales_older_grants) {
  re_test::StoreFixture fixture;
  const Revision after_first = fixture.publish_authority(1u);
  const EntitlementId id = fixture.grant_default("stale-capacity", 10u);
  RE_CHECK(fixture.store().verify(plain_verify(id, fixture.now()))->authorized);

  entl::AuthorityUpdateRequest update = re_test::authority_request(
      "capacity-bump", fixture.now(), after_first, 2u);
  RE_REQUIRE_OK(fixture.store().update_authority(update));

  auto decision = fixture.store().verify(plain_verify(id, fixture.now()));
  RE_REQUIRE(decision.has_value());
  RE_CHECK(!decision->authorized);
  RE_CHECK_EQ(decision->primary, ErrorCode::kStaleBinding);
  RE_CHECK(decision->stale);

  // A draw against stale authority is refused with the same diagnostic.
  entl::DrawRequest draw;
  draw.request_id = re_test::request_id("stale-draw");
  draw.now = fixture.now();
  draw.id = id;
  draw.expected_revision = fixture.store().get(id)->revision;
  draw.quantity = entl::Quantity::make(Unit::kCount, 1u).value();
  draw.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_CHECK_ERR(fixture.store().draw(draw), ErrorCode::kStaleBinding);
}

RE_TEST(fencing, a_fence_mask_without_the_capacity_bit_keeps_authority_live) {
  re_test::StoreFixture fixture;
  const Revision after_first = fixture.publish_authority(1u);

  entl::GrantRequest grant = fixture.make_grant("unfenced-capacity", 10u);
  grant.fence = entl::FenceMask::control_epoch_only();
  const EntitlementId id = fixture.grant_full(grant);
  RE_CHECK(fixture.store().verify(plain_verify(id, fixture.now()))->authorized);

  entl::AuthorityUpdateRequest update = re_test::authority_request(
      "capacity-bump-2", fixture.now(), after_first, 2u);
  RE_REQUIRE_OK(fixture.store().update_authority(update));
  RE_CHECK(fixture.store().verify(plain_verify(id, fixture.now()))->authorized);

  // The control epoch is mandatory and is always fenced.
  entl::AuthorityUpdateRequest epoch_update = re_test::authority_request(
      "epoch-bump", fixture.now(), fixture.store().authority().revision, 2u);
  epoch_update.advance_epoch = true;
  RE_REQUIRE_OK(fixture.store().update_authority(epoch_update));
  auto decision = fixture.store().verify(plain_verify(id, fixture.now()));
  RE_REQUIRE(decision.has_value());
  RE_CHECK_EQ(decision->primary, ErrorCode::kStaleEpoch);
}

RE_TEST(fencing, an_epoch_advance_fences_every_live_entitlement_atomically) {
  re_test::StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId first = fixture.grant_default("epoch-one", 10u);
  const EntitlementId second = fixture.grant_default("epoch-two", 20u);

  entl::AuthorityUpdateRequest update = re_test::authority_request(
      "epoch-advance", fixture.now(), fixture.store().authority().revision, 1u);
  update.advance_epoch = true;
  const std::uint64_t before = fixture.store().commit_seq().value();
  RE_REQUIRE_OK(fixture.store().update_authority(update));
  RE_CHECK_EQ(fixture.store().commit_seq().value(), before + 1u);

  const auto first_decision = fixture.store().verify(plain_verify(first, fixture.now()));
  const auto second_decision = fixture.store().verify(plain_verify(second, fixture.now()));
  RE_REQUIRE(first_decision.has_value());
  RE_REQUIRE(second_decision.has_value());
  RE_CHECK_EQ(first_decision->primary, ErrorCode::kStaleEpoch);
  RE_CHECK_EQ(second_decision->primary, ErrorCode::kStaleEpoch);
  RE_CHECK_EQ(first_decision->control_epoch.value(), fixture.store().authority().control_epoch.value());
}

RE_TEST(fencing, reissue_rebinds_existing_evidence_to_current_authority) {
  re_test::StoreFixture fixture;
  const Revision after_first = fixture.publish_authority(1u);
  const EntitlementId source = fixture.grant_default("rebind-source", 50u);
  const entl::Sha256Digest admission = fixture.store().get(source)->binding.admission_decision_digest;

  entl::AuthorityUpdateRequest update = re_test::authority_request(
      "capacity-bump-3", fixture.now(), after_first, 4u);
  RE_REQUIRE_OK(fixture.store().update_authority(update));
  RE_CHECK_EQ(fixture.store().verify(plain_verify(source, fixture.now()))->primary, ErrorCode::kStaleBinding);

  entl::ReissueRequest request;
  request.request_id = re_test::request_id("rebind-op");
  request.now = fixture.now();
  request.id = source;
  request.expected_revision = fixture.store().get(source)->revision;
  request.actor = re_test::must_parse<entl::ActorId>("operator-1");
  request.rebind_to_current_authority = true;
  auto outcome = fixture.store().reissue(request);
  RE_REQUIRE(outcome.has_value());

  auto successor = fixture.store().get(outcome->primary_id);
  RE_REQUIRE(successor.has_value());
  RE_CHECK_EQ(successor->binding.facility_capacity_generation.value(), std::uint64_t{4});
  RE_CHECK_EQ(successor->binding.admission_decision_digest.to_hex(), admission.to_hex());
  RE_CHECK(fixture.store().verify(plain_verify(successor->id, fixture.now()))->authorized);

  // Keeping the old binding deliberately produces a still-stale successor.
  const EntitlementId second_source = fixture.grant_default("rebind-source-2", 20u);
  entl::AuthorityUpdateRequest second_update = re_test::authority_request(
      "capacity-bump-4", fixture.now(), fixture.store().authority().revision, 5u);
  RE_REQUIRE_OK(fixture.store().update_authority(second_update));
  entl::ReissueRequest keep;
  keep.request_id = re_test::request_id("rebind-keep");
  keep.now = fixture.now();
  keep.id = second_source;
  keep.expected_revision = fixture.store().get(second_source)->revision;
  keep.actor = re_test::must_parse<entl::ActorId>("operator-1");
  keep.rebind_to_current_authority = false;
  auto kept = fixture.store().reissue(keep);
  RE_REQUIRE(kept.has_value());
  RE_CHECK_EQ(fixture.store().verify(plain_verify(kept->primary_id, fixture.now()))->primary,
              ErrorCode::kStaleBinding);
}

RE_TEST(fencing, restart_preserves_authority_when_the_design_says_it_is_portable) {
  re_test::TempDirectory holder("restart");
  EntitlementId id;
  entl::Timestamp later;

  {
    auto opened = re_test::open_store_at(holder.path(), true);
    RE_REQUIRE(opened.has_value());
    entl::Store& store = *opened.value();
    entl::AuthorityUpdateRequest update = re_test::authority_request(
        "restart-authority", re_test::instant(1700000000), Revision::from_value(1u), 3u);
    RE_REQUIRE_OK(store.update_authority(update));
    entl::GrantRequest grant;
    grant.request_id = re_test::request_id("restart-grant");
    grant.now = re_test::instant(1700000001);
    grant.holder = re_test::must_parse<entl::TenantId>("tenant-a");
    grant.service = re_test::must_parse<entl::ServiceId>("inference");
    grant.service_class = re_test::must_parse<entl::ServiceClassId>("gold");
    grant.scope.facility = re_test::must_parse<entl::FacilityId>("dc-1");
    grant.scope.resource_type = re_test::must_parse<entl::ResourceTypeId>("accelerator");
    grant.scope.scope = re_test::must_parse<entl::ResourceScopeId>("scope-a");
    grant.unit = Unit::kCount;
    grant.quantity = entl::Quantity::make(Unit::kCount, 25u).value();
    grant.effective_from = re_test::instant(1699999000);
    grant.expires_at = re_test::instant(1799999000);
    grant.admission_decision_digest = re_test::digest_of("restart-admission");
    grant.fence = entl::FenceMask::standard();
    grant.actor = re_test::must_parse<entl::ActorId>("operator-1");
    auto outcome = store.grant(grant);
    RE_REQUIRE(outcome.has_value());
    id = outcome->primary_id;
    later = re_test::instant(1700000002);
  }

  auto reopened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(reopened.has_value());
  entl::Store& store = *reopened.value();
  RE_CHECK_EQ(store.authority().facility_capacity_generation.value(), std::uint64_t{3});
  RE_CHECK_EQ(store.commit_seq().value(), std::uint64_t{2});
  RE_CHECK(store.verify(plain_verify(id, later))->authorized);
  RE_CHECK_EQ(store.get(id)->remaining.units(), std::uint64_t{25});
}

RE_TEST(fencing, fence_on_open_makes_previously_granted_authority_stale) {
  re_test::TempDirectory holder("fence-on-open");
  EntitlementId id;
  const entl::Timestamp later = re_test::instant(1700000002);

  {
    auto opened = re_test::open_store_at(holder.path(), true);
    RE_REQUIRE(opened.has_value());
    entl::Store& store = *opened.value();
    RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
        "fence-authority", re_test::instant(1700000000), Revision::from_value(1u), 2u)));
    entl::GrantRequest grant;
    grant.request_id = re_test::request_id("fence-grant");
    grant.now = re_test::instant(1700000001);
    grant.holder = re_test::must_parse<entl::TenantId>("tenant-a");
    grant.service = re_test::must_parse<entl::ServiceId>("inference");
    grant.service_class = re_test::must_parse<entl::ServiceClassId>("gold");
    grant.scope.facility = re_test::must_parse<entl::FacilityId>("dc-1");
    grant.scope.resource_type = re_test::must_parse<entl::ResourceTypeId>("accelerator");
    grant.scope.scope = re_test::must_parse<entl::ResourceScopeId>("scope-a");
    grant.unit = Unit::kCount;
    grant.quantity = entl::Quantity::make(Unit::kCount, 5u).value();
    grant.effective_from = re_test::instant(1699999000);
    grant.expires_at = re_test::instant(1799999000);
    grant.admission_decision_digest = re_test::digest_of("fence-admission");
    grant.fence = entl::FenceMask::standard();
    grant.actor = re_test::must_parse<entl::ActorId>("operator-1");
    auto outcome = store.grant(grant);
    RE_REQUIRE(outcome.has_value());
    id = outcome->primary_id;
  }

  auto reopened = re_test::open_store_at(holder.path(), false, true);
  RE_REQUIRE(reopened.has_value());
  entl::Store& store = *reopened.value();
  auto decision = store.verify(plain_verify(id, later));
  RE_REQUIRE(decision.has_value());
  RE_CHECK(!decision->authorized);
  RE_CHECK_EQ(decision->primary, ErrorCode::kStaleEpoch);
  RE_CHECK(decision->stale);
  RE_CHECK(store.authority().control_epoch.value() >= 2u);

  // The record itself is untouched: only its authority was fenced.
  auto record = store.get(id);
  RE_REQUIRE(record.has_value());
  RE_CHECK_EQ(record->remaining.units(), std::uint64_t{5});
}

RE_TEST(fencing, repeated_fence_on_open_advances_the_epoch_each_time) {
  re_test::TempDirectory holder("fence-repeat");
  {
    auto opened = re_test::open_store_at(holder.path(), true);
    RE_REQUIRE(opened.has_value());
    entl::Store& store = *opened.value();
    RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
        "fence-repeat-authority", re_test::instant(1700000000), Revision::from_value(1u), 1u)));
    entl::GrantRequest grant;
    grant.request_id = re_test::request_id("fence-repeat-grant");
    grant.now = re_test::instant(1700000001);
    grant.holder = re_test::must_parse<entl::TenantId>("tenant-a");
    grant.service = re_test::must_parse<entl::ServiceId>("inference");
    grant.service_class = re_test::must_parse<entl::ServiceClassId>("gold");
    grant.scope.facility = re_test::must_parse<entl::FacilityId>("dc-1");
    grant.scope.resource_type = re_test::must_parse<entl::ResourceTypeId>("accelerator");
    grant.scope.scope = re_test::must_parse<entl::ResourceScopeId>("scope-a");
    grant.unit = Unit::kCount;
    grant.quantity = entl::Quantity::make(Unit::kCount, 5u).value();
    grant.effective_from = re_test::instant(1699999000);
    grant.expires_at = re_test::instant(1799999000);
    grant.admission_decision_digest = re_test::digest_of("fence-repeat-admission");
    grant.fence = entl::FenceMask::standard();
    grant.actor = re_test::must_parse<entl::ActorId>("operator-1");
    RE_REQUIRE_OK(store.grant(grant));
  }
  std::uint64_t first_epoch = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto reopened = re_test::open_store_at(holder.path(), false, true);
    RE_REQUIRE(reopened.has_value());
    const std::uint64_t epoch = reopened.value()->authority().control_epoch.value();
    if (attempt == 0) {
      first_epoch = epoch;
    }
    RE_CHECK_EQ(epoch, first_epoch + static_cast<std::uint64_t>(attempt));
  }
}
