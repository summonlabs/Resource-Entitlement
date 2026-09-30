// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Independent out-of-tree consumer of the installed ResourceEntitlement
// package. It drives the public API only: no internal header is included and no
// symbol outside the installed package is used.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <resource_entitlement/digest.hpp>
#include <resource_entitlement/store.hpp>
#include <resource_entitlement/token.hpp>
#include <resource_entitlement/version.hpp>

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  if (!condition) {
    std::cout << "FAIL: " << description << std::endl;
    ++failures;
  }
}

template <class T>
T required(entl::Result<T>&& result, const char* description) {
  if (!result.has_value()) {
    std::cout << "FAIL: " << description << ": " << result.error().to_string() << std::endl;
    ++failures;
    throw std::runtime_error(description);
  }
  return std::move(result).value();
}

/// Convenience for the API's factory functions, which report absence with
/// std::optional rather than with an Error.
template <class T>
T required(std::optional<T>&& value, const char* description) {
  if (!value.has_value()) {
    std::cout << "FAIL: " << description << ": value is absent" << std::endl;
    ++failures;
    throw std::runtime_error(description);
  }
  return std::move(value).value();
}

entl::Timestamp instant(std::int64_t seconds) {
  return required(entl::Timestamp::from_unix_nanos(seconds * 1000000000LL), "instant");
}

template <class T>
T identifier(const char* text) {
  return required(T::parse(text), text);
}

entl::GrantRequest make_grant(const char* seed, std::uint64_t units) {
  entl::GrantRequest request;
  request.request_id = entl::RequestId::derive(seed);
  request.now = instant(1700000001);
  request.holder = identifier<entl::TenantId>("tenant-a");
  request.service = identifier<entl::ServiceId>("inference");
  request.service_class = identifier<entl::ServiceClassId>("gold");
  request.scope.facility = identifier<entl::FacilityId>("dc-1");
  request.scope.resource_type = identifier<entl::ResourceTypeId>("accelerator");
  request.scope.scope = identifier<entl::ResourceScopeId>("pool-a");
  request.unit = entl::Unit::kCount;
  request.quantity = required(entl::Quantity::make(entl::Unit::kCount, units), "quantity");
  request.priority.klass = entl::PriorityClass::kStandard;
  request.effective_from = instant(1699999000);
  request.expires_at = instant(1799999000);
  request.admission_decision_digest = entl::Sha256::hash(seed);
  request.fence = entl::FenceMask::standard();
  request.actor = identifier<entl::ActorId>("consumer");
  return request;
}

}  // namespace

int main() {
  std::cout << "ResourceEntitlement consumer, library version " << entl::version_string() << " ("
            << entl::build_mode_string() << ")" << std::endl;

  std::error_code ec;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path(ec) / "resource-entitlement-consumer";
  std::filesystem::remove_all(directory, ec);

  entl::StoreOpenOptions options;
  options.directory = directory;
  options.create_if_missing = true;

  auto opened = required(entl::Store::open(options), "open store");
  entl::Store& store = *opened;

  // Publish an authoritative snapshot.
  entl::AuthorityUpdateRequest authority;
  authority.request_id = entl::RequestId::derive("consumer-authority");
  authority.now = instant(1700000000);
  authority.expected_snapshot_revision = store.authority().revision;
  authority.facility_capacity_generation = entl::Generation::from_value(1u);
  authority.facility_policy_revision = entl::Revision::from_value(1u);
  authority.resource_envelope_revision = entl::Revision::from_value(1u);
  authority.service_class_revision = entl::Revision::from_value(1u);
  authority.policy_context_digest = entl::Sha256::hash("policy-context");
  authority.envelope_binding_digest = entl::Sha256::hash("envelope");
  authority.publisher = identifier<entl::ActorId>("facility-capacity");
  check(store.update_authority(authority).has_value(), "publish authority");
  check(store.authority().revision.value() == 2u, "authority revision advanced");

  // Grant and verify.
  auto granted = required(store.grant(make_grant("consumer-grant", 100u)), "grant");
  const entl::EntitlementId id = granted.primary_id;
  entl::VerifyRequest verify;
  verify.id = id;
  verify.now = instant(1700000002);
  verify.requested = required(entl::Quantity::make(entl::Unit::kCount, 50u), "requested");
  auto decision = required(store.verify(verify), "verify");
  check(decision.authorized, "grant is live authority");
  check(decision.primary == entl::ErrorCode::kOk, "verification code is ok");

  // Draw, then verify the accounting.
  entl::DrawRequest draw;
  draw.request_id = entl::RequestId::derive("consumer-draw");
  draw.now = instant(1700000003);
  draw.id = id;
  draw.expected_revision = store.get(id).value().revision;
  draw.quantity = required(entl::Quantity::make(entl::Unit::kCount, 40u), "draw quantity");
  draw.actor = identifier<entl::ActorId>("consumer");
  check(store.draw(draw).has_value(), "draw accepted");
  check(store.get(id).value().remaining.units() == 60u, "remaining authority is 60");

  // Idempotent replay of an already committed request.
  auto replay = store.draw(draw);
  check(replay.has_value() && replay->replayed, "draw replay is resolved as a replay");
  check(store.get(id).value().remaining.units() == 60u, "replay did not spend again");

  // Canonical token round trip.
  auto token = required(store.issue_token(id), "issue token");
  const std::vector<std::uint8_t> encoded = token.encode();
  auto decoded = required(entl::EntitlementToken::decode(encoded), "decode token");
  check(decoded.id() == id, "token carries the entitlement identity");
  entl::VerifyTokenRequest token_request;
  token_request.now = instant(1700000004);
  token_request.token = std::span<const std::uint8_t>(encoded.data(), encoded.size());
  check(required(store.verify_token(token_request), "verify token").authorized, "token verifies");

  // Suspend, resume, split, and lineage.
  entl::SuspendRequest suspend;
  suspend.request_id = entl::RequestId::derive("consumer-suspend");
  suspend.now = instant(1700000005);
  suspend.id = id;
  suspend.expected_revision = store.get(id).value().revision;
  suspend.actor = identifier<entl::ActorId>("consumer");
  check(store.suspend(suspend).has_value(), "suspend accepted");
  entl::VerifyRequest suspended;
  suspended.id = id;
  suspended.now = instant(1700000006);
  check(required(store.verify(suspended), "verify suspended").primary == entl::ErrorCode::kSuspended,
        "suspended entitlement is refused");

  entl::ResumeRequest resume;
  resume.request_id = entl::RequestId::derive("consumer-resume");
  resume.now = instant(1700000007);
  resume.id = id;
  resume.expected_revision = store.get(id).value().revision;
  resume.actor = identifier<entl::ActorId>("consumer");
  check(store.resume(resume).has_value(), "resume accepted");

  entl::TransferRequest split;
  split.request_id = entl::RequestId::derive("consumer-split");
  split.now = instant(1700000008);
  split.source = id;
  split.expected_revision = store.get(id).value().revision;
  split.mode = entl::TransferMode::kSplit;
  split.target_holder = identifier<entl::TenantId>("tenant-b");
  split.quantity = required(entl::Quantity::make(entl::Unit::kCount, 20u), "split quantity");
  split.priority = store.get(id).value().priority;
  split.actor = identifier<entl::ActorId>("consumer");
  auto split_outcome = required(store.transfer(split), "split");
  check(store.get(id).value().remaining.units() == 40u, "source keeps 40 after split");

  auto lineage = required(store.lineage(split_outcome.primary_id), "lineage");
  check(lineage.nodes.size() == 2u, "lineage holds both records");

  entl::OrderRequest order_request;
  order_request.now = instant(1700000009);
  auto order = required(store.order_for_consumption(order_request), "order");
  check(order.entries.size() == 2u, "two live entitlements are ordered");

  // Fencing: a new capacity generation stales the existing authority.
  entl::AuthorityUpdateRequest fence;
  fence.request_id = entl::RequestId::derive("consumer-fence");
  fence.now = instant(1700000010);
  fence.expected_snapshot_revision = store.authority().revision;
  fence.facility_capacity_generation = entl::Generation::from_value(2u);
  fence.publisher = identifier<entl::ActorId>("facility-capacity");
  check(store.update_authority(fence).has_value(), "publish new capacity generation");
  entl::VerifyRequest stale;
  stale.id = id;
  stale.now = instant(1700000011);
  auto stale_decision = required(store.verify(stale), "verify stale");
  check(!stale_decision.authorized && stale_decision.primary == entl::ErrorCode::kStaleBinding,
        "new capacity generation fences the older grant");
  check(stale_decision.stale, "the decision is marked stale");

  // Compaction and durable reopen.
  check(store.compact().has_value(), "compaction");
  const std::uint64_t sequence = store.commit_seq().value();
  opened.reset();

  auto reopened = required(entl::Store::open(options), "reopen store");
  check(reopened->commit_seq().value() == sequence, "reopen reproduces the commit sequence");
  check(reopened->stats().entitlement_count == 2u, "reopen reproduces both records");
  auto inspection = required(entl::Store::inspect(directory), "inspect");
  check(inspection.opened && !inspection.rollback_detected, "inspection reports no rollback");
  check(inspection.entitlements == 2u, "inspection counts both records");
  reopened.reset();

  std::filesystem::remove_all(directory, ec);

  if (failures != 0) {
    std::cout << failures << " consumer check(s) FAILED" << std::endl;
    return 1;
  }
  std::cout << "all consumer checks passed" << std::endl;
  return 0;
}
