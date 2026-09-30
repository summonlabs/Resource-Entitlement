// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <string>
#include <vector>

#include "harness.hpp"
#include "resource_entitlement/store.hpp"
#include "test_support.hpp"

using entl::ErrorCode;
using entl::EntitlementId;
using entl::EntitlementState;
using entl::PriorityClass;
using entl::TransferMode;
using entl::Unit;
using re_test::StoreFixture;

namespace {

entl::TransferRequest transfer_request(std::string_view seed, entl::Timestamp now, const EntitlementId& source,
                                       entl::Revision expected, TransferMode mode,
                                       std::string_view target) {
  entl::TransferRequest request;
  request.request_id = re_test::request_id(seed);
  request.now = now;
  request.source = source;
  request.expected_revision = expected;
  request.mode = mode;
  request.target_holder = re_test::must_parse<entl::TenantId>(target);
  request.actor = re_test::must_parse<entl::ActorId>("operator-1");
  return request;
}

entl::Revision revision_of(entl::Store& store, const EntitlementId& id) {
  auto record = store.get(id);
  if (!record.has_value()) {
    throw std::runtime_error("revision_of: " + record.error().to_string());
  }
  return record->revision;
}

}  // namespace

RE_TEST(transfer, move_changes_holder_and_preserves_history) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId id = fixture.grant_default("move-source", 40u);

  entl::TransferRequest request = transfer_request("move-op", fixture.now(), id,
                                                   revision_of(fixture.store(), id), TransferMode::kMove,
                                                   "tenant-b");
  auto outcome = fixture.store().transfer(request);
  RE_REQUIRE(outcome.has_value());
  RE_CHECK_EQ(outcome->primary_id.to_hex(), id.to_hex());
  RE_CHECK_EQ(outcome->affected.size(), std::size_t{1});

  auto record = fixture.store().get(id);
  RE_REQUIRE(record.has_value());
  RE_CHECK_EQ(record->holder.str(), std::string("tenant-b"));
  RE_CHECK_EQ(record->origin_holder.str(), std::string("tenant-a"));
  RE_REQUIRE(record->holder_history.size() == 1u);
  RE_CHECK_EQ(record->holder_history.front().from.str(), std::string("tenant-a"));
  RE_CHECK_EQ(record->holder_history.front().to.str(), std::string("tenant-b"));
  RE_CHECK_EQ(record->remaining.units(), std::uint64_t{40});
  RE_CHECK_EQ(record->granted.units(), std::uint64_t{40});

  // The previous holder no longer holds authority.
  entl::VerifyRequest verify;
  verify.id = id;
  verify.now = fixture.now();
  entl::ExpectedContext context;
  context.holder = re_test::must_parse<entl::TenantId>("tenant-a");
  context.service = re_test::must_parse<entl::ServiceId>("inference");
  context.service_class = re_test::must_parse<entl::ServiceClassId>("gold");
  context.scope.facility = re_test::must_parse<entl::FacilityId>("dc-1");
  context.scope.resource_type = re_test::must_parse<entl::ResourceTypeId>("accelerator");
  context.scope.scope = re_test::must_parse<entl::ResourceScopeId>("scope-a");
  context.unit = Unit::kCount;
  verify.expected = context;
  auto decision = fixture.store().verify(verify);
  RE_REQUIRE(decision.has_value());
  RE_CHECK_EQ(decision->primary, ErrorCode::kTenantMismatch);
  RE_CHECK(!decision->authorized);

  context.holder = re_test::must_parse<entl::TenantId>("tenant-b");
  verify.expected = context;
  RE_CHECK(fixture.store().verify(verify)->authorized);

  auto lineage = fixture.store().lineage(id);
  RE_REQUIRE(lineage.has_value());
  RE_CHECK_EQ(lineage->holder_history.size(), std::size_t{1});
}

RE_TEST(transfer, move_refuses_same_holder_and_partial_quantity) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId id = fixture.grant_default("move-guard", 10u);

  entl::TransferRequest same = transfer_request("move-same", fixture.now(), id, revision_of(fixture.store(), id),
                                                TransferMode::kMove, "tenant-a");
  RE_CHECK_ERR(fixture.store().transfer(same), ErrorCode::kSelfReference);

  entl::TransferRequest partial = transfer_request("move-partial", fixture.now(), id, revision_of(fixture.store(), id),
                                                   TransferMode::kMove, "tenant-b");
  partial.quantity = entl::Quantity::make(Unit::kCount, 5u).value();
  RE_CHECK_ERR(fixture.store().transfer(partial), ErrorCode::kUnexpectedField);

  entl::TransferRequest missing = transfer_request("split-missing", fixture.now(), id,
                                                   revision_of(fixture.store(), id), TransferMode::kSplit,
                                                   "tenant-b");
  RE_CHECK_ERR(fixture.store().transfer(missing), ErrorCode::kMissingField);

  entl::TransferRequest unknown_mode = transfer_request("unknown-mode", fixture.now(), id,
                                                        revision_of(fixture.store(), id),
                                                        static_cast<TransferMode>(9u), "tenant-b");
  RE_CHECK_ERR(fixture.store().transfer(unknown_mode), ErrorCode::kInvalidEnumValue);
}

RE_TEST(transfer, split_moves_authority_without_duplicating_it) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId source = fixture.grant_default("split-source", 100u);

  entl::TransferRequest request = transfer_request("split-op", fixture.now(), source,
                                                   revision_of(fixture.store(), source), TransferMode::kSplit,
                                                   "tenant-b");
  request.quantity = entl::Quantity::make(Unit::kCount, 30u).value();
  request.priority = fixture.store().get(source)->priority;
  auto outcome = fixture.store().transfer(request);
  RE_REQUIRE(outcome.has_value());
  const EntitlementId child = outcome->primary_id;
  RE_CHECK_NE(child.to_hex(), source.to_hex());
  RE_CHECK_EQ(outcome->affected.size(), std::size_t{2});

  auto source_record = fixture.store().get(source);
  auto child_record = fixture.store().get(child);
  RE_REQUIRE(source_record.has_value());
  RE_REQUIRE(child_record.has_value());
  RE_CHECK_EQ(source_record->granted.units(), std::uint64_t{70});
  RE_CHECK_EQ(source_record->remaining.units(), std::uint64_t{70});
  RE_CHECK_EQ(child_record->granted.units(), std::uint64_t{30});
  RE_CHECK_EQ(child_record->remaining.units(), std::uint64_t{30});
  RE_CHECK_EQ(child_record->relation, entl::LineageRelation::kSplitFrom);
  RE_CHECK_EQ(child_record->root.to_hex(), source.to_hex());
  RE_REQUIRE(child_record->sources.size() == 1u);
  RE_CHECK_EQ(child_record->sources.front().to_hex(), source.to_hex());
  RE_CHECK_EQ(child_record->holder.str(), std::string("tenant-b"));
  RE_CHECK_EQ(child_record->expires_at.unix_nanos(), source_record->expires_at.unix_nanos());

  // Total authority across the lineage is conserved exactly.
  auto lineage = fixture.store().lineage(child);
  RE_REQUIRE(lineage.has_value());
  std::uint64_t total = 0;
  for (const entl::LineageNode& node : lineage->nodes) {
    total += node.remaining.units();
  }
  RE_CHECK_EQ(total, std::uint64_t{100});
}

RE_TEST(transfer, delegate_keeps_granted_but_removes_usable_authority) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId source = fixture.grant_default("delegate-source", 80u);

  entl::TransferRequest request = transfer_request("delegate-op", fixture.now(), source,
                                                   revision_of(fixture.store(), source),
                                                   TransferMode::kDelegate, "tenant-b");
  request.quantity = entl::Quantity::make(Unit::kCount, 25u).value();
  request.priority = fixture.store().get(source)->priority;
  auto outcome = fixture.store().transfer(request);
  RE_REQUIRE(outcome.has_value());
  const EntitlementId child = outcome->primary_id;

  auto source_record = fixture.store().get(source);
  auto child_record = fixture.store().get(child);
  RE_REQUIRE(source_record.has_value());
  RE_REQUIRE(child_record.has_value());
  RE_CHECK_EQ(source_record->granted.units(), std::uint64_t{80});
  RE_CHECK_EQ(source_record->remaining.units(), std::uint64_t{55});
  RE_CHECK_EQ(source_record->delegated_out.units(), std::uint64_t{25});
  RE_CHECK_EQ(child_record->relation, entl::LineageRelation::kDelegatedFrom);

  // A draw beyond the remaining authority must fail: delegation removed it.
  entl::DrawRequest draw;
  draw.request_id = re_test::request_id("delegate-overdraw");
  draw.now = fixture.now();
  draw.id = source;
  draw.expected_revision = source_record->revision;
  draw.quantity = entl::Quantity::make(Unit::kCount, 56u).value();
  draw.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_CHECK_ERR(fixture.store().draw(draw), ErrorCode::kQuantityExceeded);
}

RE_TEST(transfer, split_refuses_more_than_remaining_and_higher_priority) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId source = fixture.grant_default("split-guard", 10u);

  entl::TransferRequest too_much = transfer_request("split-too-much", fixture.now(), source,
                                                    revision_of(fixture.store(), source), TransferMode::kSplit,
                                                    "tenant-b");
  too_much.quantity = entl::Quantity::make(Unit::kCount, 11u).value();
  RE_CHECK_ERR(fixture.store().transfer(too_much), ErrorCode::kQuantityExceeded);

  entl::TransferRequest wrong_unit = transfer_request("split-wrong-unit", fixture.now(), source,
                                                      revision_of(fixture.store(), source), TransferMode::kSplit,
                                                      "tenant-b");
  wrong_unit.quantity = entl::Quantity::make(Unit::kGibibytes, 1u).value();
  RE_CHECK_ERR(fixture.store().transfer(wrong_unit), ErrorCode::kUnitMismatch);

  entl::TransferRequest escalated = transfer_request("split-escalate", fixture.now(), source,
                                                     revision_of(fixture.store(), source), TransferMode::kSplit,
                                                     "tenant-b");
  escalated.quantity = entl::Quantity::make(Unit::kCount, 5u).value();
  escalated.priority.klass = PriorityClass::kGuaranteed;
  RE_CHECK_ERR(fixture.store().transfer(escalated), ErrorCode::kInvalidPriority);

  entl::TransferRequest deferred = escalated;
  deferred.request_id = re_test::request_id("split-defer");
  deferred.priority.klass = PriorityClass::kBackground;
  RE_REQUIRE_OK(fixture.store().transfer(deferred));
}

RE_TEST(transfer, retry_after_lost_response_does_not_transfer_twice) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId source = fixture.grant_default("retry-source", 100u);
  const entl::Revision revision = revision_of(fixture.store(), source);

  entl::TransferRequest request = transfer_request("retry-op", fixture.now(), source, revision,
                                                   TransferMode::kSplit, "tenant-b");
  request.quantity = entl::Quantity::make(Unit::kCount, 40u).value();
  request.priority = fixture.store().get(source)->priority;
  auto first = fixture.store().transfer(request);
  RE_REQUIRE(first.has_value());
  const std::uint64_t commits_after_first = fixture.store().commit_seq().value();

  // The caller never saw the response and retries the identical request, even
  // though its expected revision is now stale.
  auto retry = fixture.store().transfer(request);
  RE_REQUIRE(retry.has_value());
  RE_CHECK(retry->replayed);
  RE_CHECK_EQ(retry->primary_id.to_hex(), first->primary_id.to_hex());
  RE_CHECK_EQ(fixture.store().commit_seq().value(), commits_after_first);
  RE_CHECK_EQ(fixture.store().get(source)->remaining.units(), std::uint64_t{60});

  // A different request identity with the stale revision is refused outright.
  entl::TransferRequest second = transfer_request("retry-op-2", fixture.now(), source, revision,
                                                  TransferMode::kSplit, "tenant-c");
  second.quantity = entl::Quantity::make(Unit::kCount, 10u).value();
  second.priority = fixture.store().get(source)->priority;
  RE_CHECK_ERR(fixture.store().transfer(second), ErrorCode::kRevisionConflict);
  RE_CHECK_EQ(fixture.store().get(source)->remaining.units(), std::uint64_t{60});
}

RE_TEST(transfer, revoking_a_parent_cascades_to_delegated_children_only) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId parent = fixture.grant_default("cascade-parent", 100u);

  entl::TransferRequest delegate = transfer_request("cascade-delegate", fixture.now(), parent,
                                                    revision_of(fixture.store(), parent),
                                                    TransferMode::kDelegate, "tenant-b");
  delegate.quantity = entl::Quantity::make(Unit::kCount, 20u).value();
  delegate.priority = fixture.store().get(parent)->priority;
  auto delegated = fixture.store().transfer(delegate);
  RE_REQUIRE(delegated.has_value());

  entl::TransferRequest split = transfer_request("cascade-split", fixture.now(), parent,
                                                 revision_of(fixture.store(), parent), TransferMode::kSplit,
                                                 "tenant-c");
  split.quantity = entl::Quantity::make(Unit::kCount, 20u).value();
  split.priority = fixture.store().get(parent)->priority;
  auto split_child = fixture.store().transfer(split);
  RE_REQUIRE(split_child.has_value());

  entl::RevokeRequest revoke;
  revoke.request_id = re_test::request_id("cascade-revoke");
  revoke.now = fixture.now();
  revoke.id = parent;
  revoke.expected_revision = revision_of(fixture.store(), parent);
  revoke.actor = re_test::must_parse<entl::ActorId>("operator-1");
  revoke.cascade_delegated_children = true;
  auto outcome = fixture.store().revoke(revoke);
  RE_REQUIRE(outcome.has_value());
  RE_CHECK_EQ(outcome->affected.size(), std::size_t{2});

  auto parent_record = fixture.store().get(parent);
  auto delegated_record = fixture.store().get(delegated->primary_id);
  auto split_record = fixture.store().get(split_child->primary_id);
  RE_REQUIRE(parent_record.has_value());
  RE_REQUIRE(delegated_record.has_value());
  RE_REQUIRE(split_record.has_value());
  RE_CHECK_EQ(parent_record->state, EntitlementState::kRevoked);
  RE_CHECK_EQ(delegated_record->state, EntitlementState::kRevoked);
  RE_CHECK_EQ(delegated_record->terminal_reason, entl::TerminalReason::kParentRevoked);
  RE_CHECK_EQ(split_record->state, EntitlementState::kActive);

  // Without the cascade flag the delegated child survives the revocation.
  const EntitlementId second_parent = fixture.grant_default("cascade-parent-2", 40u);
  entl::TransferRequest second_delegate =
      transfer_request("cascade-delegate-2", fixture.now(), second_parent,
                       revision_of(fixture.store(), second_parent), TransferMode::kDelegate, "tenant-b");
  second_delegate.quantity = entl::Quantity::make(Unit::kCount, 10u).value();
  second_delegate.priority = fixture.store().get(second_parent)->priority;
  auto second_child = fixture.store().transfer(second_delegate);
  RE_REQUIRE(second_child.has_value());
  entl::RevokeRequest no_cascade;
  no_cascade.request_id = re_test::request_id("cascade-revoke-2");
  no_cascade.now = fixture.now();
  no_cascade.id = second_parent;
  no_cascade.expected_revision = revision_of(fixture.store(), second_parent);
  no_cascade.actor = re_test::must_parse<entl::ActorId>("operator-1");
  no_cascade.cascade_delegated_children = false;
  RE_REQUIRE_OK(fixture.store().revoke(no_cascade));
  RE_CHECK_EQ(fixture.store().get(second_child->primary_id)->state, EntitlementState::kActive);
}

RE_TEST(merge, combines_authority_and_supersedes_both_sources) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId first = fixture.grant_default("merge-a", 30u);
  const EntitlementId second = fixture.grant_default("merge-b", 20u);

  entl::MergeRequest request;
  request.request_id = re_test::request_id("merge-op");
  request.now = fixture.now();
  request.first = first;
  request.first_expected_revision = revision_of(fixture.store(), first);
  request.second = second;
  request.second_expected_revision = revision_of(fixture.store(), second);
  request.actor = re_test::must_parse<entl::ActorId>("operator-1");
  auto outcome = fixture.store().merge(request);
  RE_REQUIRE(outcome.has_value());
  RE_CHECK_EQ(outcome->affected.size(), std::size_t{3});

  auto merged = fixture.store().get(outcome->primary_id);
  RE_REQUIRE(merged.has_value());
  RE_CHECK_EQ(merged->granted.units(), std::uint64_t{50});
  RE_CHECK_EQ(merged->remaining.units(), std::uint64_t{50});
  RE_CHECK_EQ(merged->relation, entl::LineageRelation::kMergedFrom);
  RE_CHECK_EQ(merged->sources.size(), std::size_t{2});
  RE_CHECK_EQ(merged->priority, fixture.store().get(first)->priority);
  RE_CHECK(!(merged->expires_at < fixture.store().get(first)->expires_at));
  RE_CHECK(merged->expires_at <= fixture.store().get(first)->expires_at);

  RE_CHECK_EQ(fixture.store().get(first)->state, EntitlementState::kSuperseded);
  RE_CHECK_EQ(fixture.store().get(first)->terminal_reason, entl::TerminalReason::kSupersededByMerge);
  RE_CHECK_EQ(fixture.store().get(second)->state, EntitlementState::kSuperseded);

  auto lineage = fixture.store().lineage(merged->id);
  RE_REQUIRE(lineage.has_value());
  RE_CHECK_EQ(lineage->nodes.size(), std::size_t{3});
  for (const entl::LineageNode& node : lineage->nodes) {
    if (node.id == merged->id) {
      RE_CHECK(node.live);
    } else {
      RE_CHECK(!node.live);
    }
  }
}

RE_TEST(merge, refuses_incompatible_pairs) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId first = fixture.grant_default("merge-incompatible-a", 10u);
  const EntitlementId second = fixture.grant_default("merge-incompatible-b", 10u, "tenant-b");
  const EntitlementId third = fixture.grant_default("merge-incompatible-c", 10u, "tenant-a", "scope-z");

  const auto attempt = [&](const EntitlementId& a, const EntitlementId& b, const char* seed) {
    entl::MergeRequest request;
    request.request_id = re_test::request_id(seed);
    request.now = fixture.now();
    request.first = a;
    request.first_expected_revision = revision_of(fixture.store(), a);
    request.second = b;
    request.second_expected_revision = revision_of(fixture.store(), b);
    request.actor = re_test::must_parse<entl::ActorId>("operator-1");
    return fixture.store().merge(request);
  };

  RE_CHECK_ERR(attempt(first, second, "merge-tenant"), ErrorCode::kTenantMismatch);
  RE_CHECK_ERR(attempt(first, third, "merge-scope"), ErrorCode::kScopeMismatch);
  RE_CHECK_ERR(attempt(first, first, "merge-self"), ErrorCode::kSelfReference);

  // Exhausted or suspended sources cannot be merged.
  entl::SuspendRequest suspend;
  suspend.request_id = re_test::request_id("merge-suspend");
  suspend.now = fixture.now();
  suspend.id = third;
  suspend.expected_revision = revision_of(fixture.store(), third);
  suspend.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().suspend(suspend));
  RE_CHECK_ERR(attempt(first, third, "merge-suspended"), ErrorCode::kSuspended);
}

RE_TEST(reissue, mints_a_successor_and_supersedes_the_source) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId source = fixture.grant_default("reissue-source", 60u);

  entl::DrawRequest draw;
  draw.request_id = re_test::request_id("reissue-draw");
  draw.now = fixture.now();
  draw.id = source;
  draw.expected_revision = revision_of(fixture.store(), source);
  draw.quantity = entl::Quantity::make(Unit::kCount, 20u).value();
  draw.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().draw(draw));

  entl::ReissueRequest request;
  request.request_id = re_test::request_id("reissue-op");
  request.now = fixture.now();
  request.id = source;
  request.expected_revision = revision_of(fixture.store(), source);
  request.actor = re_test::must_parse<entl::ActorId>("operator-1");
  auto outcome = fixture.store().reissue(request);
  RE_REQUIRE(outcome.has_value());
  const EntitlementId successor = outcome->primary_id;
  RE_CHECK_NE(successor.to_hex(), source.to_hex());

  auto source_record = fixture.store().get(source);
  auto successor_record = fixture.store().get(successor);
  RE_REQUIRE(source_record.has_value());
  RE_REQUIRE(successor_record.has_value());
  RE_CHECK_EQ(source_record->state, EntitlementState::kSuperseded);
  RE_CHECK_EQ(source_record->terminal_reason, entl::TerminalReason::kSupersededByReissue);
  RE_CHECK_EQ(successor_record->granted.units(), std::uint64_t{40});
  RE_CHECK_EQ(successor_record->remaining.units(), std::uint64_t{40});
  RE_CHECK_EQ(successor_record->expires_at.unix_nanos(), source_record->expires_at.unix_nanos());
  RE_CHECK_EQ(successor_record->holder.str(), source_record->holder.str());
  RE_CHECK_EQ(successor_record->relation, entl::LineageRelation::kReissuedFrom);

  // The superseded record confers no authority even though it still has
  // remaining quantity recorded.
  entl::VerifyRequest verify;
  verify.id = source;
  verify.now = fixture.now();
  auto decision = fixture.store().verify(verify);
  RE_REQUIRE(decision.has_value());
  RE_CHECK_EQ(decision->primary, ErrorCode::kSuperseded);

  // The identical request is resolved as a replay, not re-evaluated.
  auto replay = fixture.store().reissue(request);
  RE_REQUIRE(replay.has_value());
  RE_CHECK(replay->replayed);
  RE_CHECK_EQ(replay->primary_id.to_hex(), successor.to_hex());
}

RE_TEST(reissue, refuses_terminal_and_exhausted_records) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId id = fixture.grant_default("reissue-guard", 10u);

  entl::DrawRequest drain;
  drain.request_id = re_test::request_id("reissue-drain");
  drain.now = fixture.now();
  drain.id = id;
  drain.expected_revision = revision_of(fixture.store(), id);
  drain.quantity = entl::Quantity::make(Unit::kCount, 10u).value();
  drain.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().draw(drain));

  entl::ReissueRequest request;
  request.request_id = re_test::request_id("reissue-exhausted");
  request.now = fixture.now();
  request.id = id;
  request.expected_revision = revision_of(fixture.store(), id);
  request.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_CHECK_ERR(fixture.store().reissue(request), ErrorCode::kQuantityExceeded);
}

RE_TEST(double_spend, retried_draw_cannot_spend_twice) {
  StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const EntitlementId id = fixture.grant_default("double-spend", 10u);

  entl::DrawRequest draw;
  draw.request_id = re_test::request_id("double-spend-draw");
  draw.now = fixture.now();
  draw.id = id;
  draw.expected_revision = revision_of(fixture.store(), id);
  draw.quantity = entl::Quantity::make(Unit::kCount, 10u).value();
  draw.actor = re_test::must_parse<entl::ActorId>("operator-1");
  auto first = fixture.store().draw(draw);
  RE_REQUIRE(first.has_value());
  RE_CHECK_EQ(fixture.store().get(id)->remaining.units(), std::uint64_t{0});

  auto replay = fixture.store().draw(draw);
  RE_REQUIRE(replay.has_value());
  RE_CHECK(replay->replayed);
  RE_CHECK_EQ(fixture.store().get(id)->remaining.units(), std::uint64_t{0});
  RE_CHECK_EQ(fixture.store().commit_seq().value(), first->commit_seq.value());

  // Even a fresh request identity at the current revision cannot draw again:
  // there is no authority left.
  entl::DrawRequest again = draw;
  again.request_id = re_test::request_id("double-spend-draw-2");
  again.expected_revision = fixture.store().get(id)->revision;
  RE_CHECK_ERR(fixture.store().draw(again), ErrorCode::kQuantityExceeded);

  // A stale retry with a fresh identity is refused on the revision instead, so
  // neither path can spend twice.
  entl::DrawRequest stale = draw;
  stale.request_id = re_test::request_id("double-spend-draw-3");
  RE_CHECK_ERR(fixture.store().draw(stale), ErrorCode::kRevisionConflict);
}
