// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <string>
#include <vector>

#include "harness.hpp"
#include "resource_entitlement/store.hpp"
#include "resource_entitlement/token.hpp"
#include "test_support.hpp"

using entl::ErrorCode;
using entl::EntitlementId;
using entl::PriorityClass;
using entl::Unit;
using re_test::StoreFixture;

RE_TEST(query, list_applies_every_filter_deterministically) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId alpha = fixture.grant_default("list-alpha", 10u, "tenant-a", "scope-a");
  const EntitlementId beta = fixture.grant_default("list-beta", 20u, "tenant-b", "scope-a");
  const EntitlementId gamma = fixture.grant_default("list-gamma", 30u, "tenant-a", "scope-b");

  entl::ListFilter all;
  auto everything = fixture.store().list(all);
  RE_REQUIRE(everything.has_value());
  RE_CHECK_EQ(everything->size(), std::size_t{3});
  RE_CHECK(std::is_sorted(everything->begin(), everything->end(),
                          [](const entl::Entitlement& a, const entl::Entitlement& b) { return a.id < b.id; }));

  entl::ListFilter by_tenant;
  by_tenant.holder = re_test::must_parse<entl::TenantId>("tenant-a");
  auto tenant_list = fixture.store().list(by_tenant);
  RE_REQUIRE(tenant_list.has_value());
  RE_CHECK_EQ(tenant_list->size(), std::size_t{2});

  entl::ListFilter by_scope;
  by_scope.scope = fixture.store().get(alpha)->scope;
  auto scope_list = fixture.store().list(by_scope);
  RE_REQUIRE(scope_list.has_value());
  RE_CHECK_EQ(scope_list->size(), std::size_t{2});
  RE_CHECK(std::all_of(scope_list->begin(), scope_list->end(), [&](const entl::Entitlement& record) {
    return record.scope.scope == fixture.store().get(alpha)->scope.scope;
  }));

  entl::ListFilter by_id;
  by_id.root = beta;
  auto id_list = fixture.store().list(by_id);
  RE_REQUIRE(id_list.has_value());
  RE_CHECK_EQ(id_list->size(), std::size_t{1});
  RE_CHECK_EQ(id_list->front().id.to_hex(), beta.to_hex());

  entl::ListFilter limited;
  limited.limit = 1;
  auto limited_list = fixture.store().list(limited);
  RE_REQUIRE(limited_list.has_value());
  RE_CHECK_EQ(limited_list->size(), std::size_t{1});

  entl::ListFilter live;
  live.live_only = true;
  live.now = fixture.now();
  auto live_list = fixture.store().list(live);
  RE_REQUIRE(live_list.has_value());
  RE_CHECK_EQ(live_list->size(), std::size_t{3});
  (void)gamma;
}

RE_TEST(query, lineage_is_ordered_and_reports_liveness_without_implying_authority) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId root = fixture.grant_default("lineage-root", 100u);
  const entl::Revision root_revision = fixture.store().get(root)->revision;

  entl::TransferRequest split;
  split.request_id = re_test::request_id("lineage-split");
  split.now = fixture.now();
  split.source = root;
  split.expected_revision = root_revision;
  split.mode = entl::TransferMode::kSplit;
  split.target_holder = re_test::must_parse<entl::TenantId>("tenant-b");
  split.quantity = entl::Quantity::make(Unit::kCount, 40u).value();
  split.priority = fixture.store().get(root)->priority;
  split.actor = re_test::must_parse<entl::ActorId>("operator-1");
  auto split_outcome = fixture.store().transfer(split);
  RE_REQUIRE(split_outcome.has_value());

  entl::RevokeRequest revoke;
  revoke.request_id = re_test::request_id("lineage-revoke-child");
  revoke.now = fixture.now();
  revoke.id = split_outcome->primary_id;
  revoke.expected_revision = fixture.store().get(split_outcome->primary_id)->revision;
  revoke.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().revoke(revoke));

  auto view = fixture.store().lineage(split_outcome->primary_id);
  RE_REQUIRE(view.has_value());
  RE_CHECK_EQ(view->root.to_hex(), root.to_hex());
  RE_CHECK_EQ(view->nodes.size(), std::size_t{2});
  RE_CHECK(view->nodes.front().created_at <= view->nodes.back().created_at);

  std::size_t live_count = 0;
  for (const entl::LineageNode& node : view->nodes) {
    if (node.live) {
      ++live_count;
      RE_CHECK_EQ(node.id.to_hex(), root.to_hex());
    } else if (node.id.to_hex() == split_outcome->primary_id.to_hex()) {
      RE_CHECK_EQ(node.liveness_code, ErrorCode::kRevoked);
      RE_CHECK_EQ(node.terminal_reason, entl::TerminalReason::kOperatorRevocation);
    }
  }
  RE_CHECK_EQ(live_count, std::size_t{1});
  bool found_child = false;
  for (const entl::LineageNode& node : view->nodes) {
    if (node.id.to_hex() == split_outcome->primary_id.to_hex()) {
      found_child = true;
      RE_CHECK_EQ(node.remaining.units(), std::uint64_t{40});
      RE_CHECK_EQ(node.relation, entl::LineageRelation::kSplitFrom);
    }
  }
  RE_CHECK(found_child);
}

RE_TEST(query, order_for_consumption_is_deterministic_and_advisory) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);

  entl::GrantRequest low = fixture.make_grant("order-low", 10u);
  low.priority.klass = PriorityClass::kBackground;
  const EntitlementId low_id = fixture.grant_full(low);

  entl::GrantRequest high = fixture.make_grant("order-high", 10u);
  high.priority.klass = PriorityClass::kGuaranteed;
  high.priority.rank = 5u;
  const EntitlementId high_id = fixture.grant_full(high);

  entl::GrantRequest middle = fixture.make_grant("order-middle", 10u);
  middle.priority.klass = PriorityClass::kStandard;
  const EntitlementId middle_id = fixture.grant_full(middle);

  entl::OrderRequest request;
  request.now = fixture.now();
  auto first = fixture.store().order_for_consumption(request);
  auto second = fixture.store().order_for_consumption(request);
  RE_REQUIRE(first.has_value());
  RE_REQUIRE(second.has_value());
  RE_REQUIRE(first->entries.size() == 3u);
  RE_CHECK_EQ(first->entries[0].id.to_hex(), high_id.to_hex());
  RE_CHECK_EQ(first->entries[1].id.to_hex(), middle_id.to_hex());
  RE_CHECK_EQ(first->entries[2].id.to_hex(), low_id.to_hex());
  for (std::size_t index = 0; index < first->entries.size(); ++index) {
    RE_CHECK_EQ(first->entries[index].id.to_hex(), second->entries[index].id.to_hex());
  }

  // Ordering never changed any authority.
  RE_CHECK_EQ(fixture.store().get(low_id)->remaining.units(), std::uint64_t{10});
  RE_CHECK_EQ(fixture.store().commit_seq().value(), fixture.store().stats().commit_seq.value());

  entl::RevokeRequest revoke;
  revoke.request_id = re_test::request_id("order-revoke");
  revoke.now = fixture.now();
  revoke.id = low_id;
  revoke.expected_revision = fixture.store().get(low_id)->revision;
  revoke.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().revoke(revoke));

  auto third = fixture.store().order_for_consumption(request);
  RE_REQUIRE(third.has_value());
  RE_CHECK_EQ(third->entries.size(), std::size_t{2});
  RE_REQUIRE(third->excluded.size() == 1u);
  RE_CHECK_EQ(third->excluded.front().id.to_hex(), low_id.to_hex());
  RE_CHECK_EQ(third->excluded.front().code, ErrorCode::kRevoked);
}

RE_TEST(query, tokens_round_trip_and_detect_a_superseded_revision) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId id = fixture.grant_default("token-target", 15u);

  auto token = fixture.store().issue_token(id);
  RE_REQUIRE(token.has_value());
  const std::vector<std::uint8_t> bytes = token->encode();

  entl::VerifyTokenRequest verify;
  verify.now = fixture.now();
  verify.token = std::span<const std::uint8_t>(bytes.data(), bytes.size());
  auto decision = fixture.store().verify_token(verify);
  RE_REQUIRE(decision.has_value());
  RE_CHECK(decision->authorized);
  RE_CHECK_EQ(decision->token_digest.to_hex(), token->token_digest().to_hex());

  // A mutation advances the record revision, so the carried token becomes
  // stale evidence even though the identity is unchanged.
  entl::DrawRequest draw;
  draw.request_id = re_test::request_id("token-draw");
  draw.now = fixture.now();
  draw.id = id;
  draw.expected_revision = fixture.store().get(id)->revision;
  draw.quantity = entl::Quantity::make(Unit::kCount, 5u).value();
  draw.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().draw(draw));

  auto stale = fixture.store().verify_token(verify);
  RE_REQUIRE(stale.has_value());
  RE_CHECK(!stale->authorized);
  RE_CHECK_EQ(stale->primary, ErrorCode::kStaleBinding);
  RE_CHECK(stale->stale);

  auto refreshed = fixture.store().issue_token(id);
  RE_REQUIRE(refreshed.has_value());
  const std::vector<std::uint8_t> refreshed_bytes = refreshed->encode();
  verify.token = std::span<const std::uint8_t>(refreshed_bytes.data(), refreshed_bytes.size());
  RE_CHECK(fixture.store().verify_token(verify)->authorized);
}

RE_TEST(query, stats_track_the_durable_ledger) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId first = fixture.grant_default("stats-a", 10u);
  const EntitlementId second = fixture.grant_default("stats-b", 10u);

  entl::RevokeRequest revoke;
  revoke.request_id = re_test::request_id("stats-revoke");
  revoke.now = fixture.now();
  revoke.id = second;
  revoke.expected_revision = fixture.store().get(second)->revision;
  revoke.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().revoke(revoke));

  const entl::StoreStats stats = fixture.store().stats();
  RE_CHECK_EQ(stats.entitlement_count, std::size_t{2});
  RE_CHECK_EQ(stats.terminal_count, std::size_t{1});
  RE_CHECK_EQ(stats.live_count, std::size_t{1});
  RE_CHECK_EQ(stats.idempotency_entries, std::size_t{4});
  RE_CHECK_EQ(stats.commit_seq.value(), std::uint64_t{4});
  RE_CHECK(stats.log_bytes > 0u);
  RE_CHECK_EQ(stats.fence_write_failures, std::uint64_t{0});
  (void)first;
}

RE_TEST(query, the_lost_response_window_is_bounded_and_evicts_oldest_first) {
  re_test::TempDirectory holder("idempotency-window");
  auto opened = re_test::open_store_at(holder.path(), true, false, 16u);
  RE_REQUIRE(opened.has_value());
  entl::Store& store = *opened.value();
  RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
      "window-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 1u)));

  const auto make = [](std::uint64_t index) {
    entl::GrantRequest grant;
    grant.request_id = entl::RequestId::derive("window-grant-" + std::to_string(index));
    grant.now = re_test::instant(1700000001);
    grant.holder = re_test::must_parse<entl::TenantId>("tenant-a");
    grant.service = re_test::must_parse<entl::ServiceId>("inference");
    grant.service_class = re_test::must_parse<entl::ServiceClassId>("gold");
    grant.scope.facility = re_test::must_parse<entl::FacilityId>("dc-1");
    grant.scope.resource_type = re_test::must_parse<entl::ResourceTypeId>("accelerator");
    grant.scope.scope = re_test::must_parse<entl::ResourceScopeId>("scope-a");
    grant.unit = Unit::kCount;
    grant.quantity = entl::Quantity::make(Unit::kCount, 1u).value();
    grant.effective_from = re_test::instant(1699999000);
    grant.expires_at = re_test::instant(1799999000);
    grant.admission_decision_digest = re_test::digest_of("window-admission-" + std::to_string(index));
    grant.fence = entl::FenceMask::standard();
    grant.actor = re_test::must_parse<entl::ActorId>("operator-1");
    return grant;
  };

  for (std::uint64_t index = 0; index < 40u; ++index) {
    RE_REQUIRE_OK(store.grant(make(index)));
  }
  RE_CHECK_EQ(store.stats().idempotency_entries, std::size_t{16});
  RE_CHECK_EQ(store.stats().entitlement_count, std::size_t{40});

  // The most recent request is still replayable.
  auto recent = store.grant(make(39u));
  RE_REQUIRE(recent.has_value());
  RE_CHECK(recent->replayed);
  RE_CHECK_EQ(store.stats().entitlement_count, std::size_t{40});

  // The oldest has been evicted, so a retry is a fresh decision. It still
  // cannot double-spend, because authority creation is additive by design and
  // the caller supplied a distinct deterministic identity.
  RE_CHECK_ERR(store.lookup_outcome(make(0u).request_id), ErrorCode::kNoRecordedOutcome);
}

RE_TEST(query, compaction_reclaims_segments_and_recovery_still_matches) {
  re_test::TempDirectory holder("compaction");
  entl::StoreOpenOptions options = re_test::test_options(holder.path(), true);
  options.max_segment_bytes = 8192u;
  options.max_journal_entry_bytes = 4096u;
  EntitlementId target;
  std::uint64_t sequence = 0;
  {
    auto opened = entl::Store::open(options);
    RE_REQUIRE(opened.has_value());
    entl::Store& store = *opened.value();
    RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
        "compact-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 1u)));
    for (std::uint64_t index = 0; index < 60u; ++index) {
      entl::GrantRequest grant;
      grant.request_id = entl::RequestId::derive("compact-grant-" + std::to_string(index));
      grant.now = re_test::instant(1700000001);
      grant.holder = re_test::must_parse<entl::TenantId>("tenant-a");
      grant.service = re_test::must_parse<entl::ServiceId>("inference");
      grant.service_class = re_test::must_parse<entl::ServiceClassId>("gold");
      grant.scope.facility = re_test::must_parse<entl::FacilityId>("dc-1");
      grant.scope.resource_type = re_test::must_parse<entl::ResourceTypeId>("accelerator");
      grant.scope.scope = re_test::must_parse<entl::ResourceScopeId>("scope-a");
      grant.unit = Unit::kCount;
      grant.quantity = entl::Quantity::make(Unit::kCount, 3u).value();
      grant.effective_from = re_test::instant(1699999000);
      grant.expires_at = re_test::instant(1799999000);
      grant.admission_decision_digest = re_test::digest_of("compact-admission-" + std::to_string(index));
      grant.fence = entl::FenceMask::standard();
      grant.actor = re_test::must_parse<entl::ActorId>("operator-1");
      auto outcome = store.grant(grant);
      RE_REQUIRE(outcome.has_value());
      if (index == 0u) {
        target = outcome->primary_id;
      }
    }
    sequence = store.commit_seq().value();
    RE_CHECK(store.stats().log_segment_count > 1u);
    auto report = store.compact();
    RE_REQUIRE(report.has_value());
    RE_CHECK(report->performed);
    RE_CHECK_EQ(report->snapshot_seq.value(), sequence);
    RE_CHECK(report->segments_removed >= 1u);
  }

  auto reopened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(reopened.has_value());
  entl::Store& store = *reopened.value();
  RE_CHECK_EQ(store.commit_seq().value(), sequence);
  RE_CHECK_EQ(store.stats().entitlement_count, std::size_t{60});
  RE_CHECK(store.verify([&] {
            entl::VerifyRequest request;
            request.id = target;
            request.now = re_test::instant(1700000002);
            return request;
          }())->authorized);

  auto inspection = entl::Store::inspect(holder.path());
  RE_REQUIRE(inspection.has_value());
  RE_CHECK(inspection->opened);
  RE_CHECK_EQ(inspection->commit_seq.value(), sequence);
  RE_CHECK(inspection->snapshot_seq.is_set());
  RE_CHECK_EQ(inspection->entitlements, std::size_t{60});
  RE_CHECK(!inspection->rollback_detected);
}
