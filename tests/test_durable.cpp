// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "harness.hpp"
#include "resource_entitlement/store.hpp"
#include "test_support.hpp"

using entl::EntitlementId;
using entl::ErrorCode;
using entl::Unit;

namespace {

entl::GrantRequest simple_grant(std::string_view seed, entl::Timestamp now, std::uint64_t units) {
  entl::GrantRequest grant;
  grant.request_id = re_test::request_id(seed);
  grant.now = now;
  grant.holder = re_test::must_parse<entl::TenantId>("tenant-a");
  grant.service = re_test::must_parse<entl::ServiceId>("inference");
  grant.service_class = re_test::must_parse<entl::ServiceClassId>("gold");
  grant.scope.facility = re_test::must_parse<entl::FacilityId>("dc-1");
  grant.scope.resource_type = re_test::must_parse<entl::ResourceTypeId>("accelerator");
  grant.scope.scope = re_test::must_parse<entl::ResourceScopeId>("scope-a");
  grant.unit = Unit::kCount;
  grant.quantity = entl::Quantity::make(Unit::kCount, units).value();
  grant.effective_from = re_test::instant(1699999000);
  grant.expires_at = re_test::instant(1799999000);
  grant.admission_decision_digest = re_test::digest_of(std::string("admission-") + std::string(seed));
  grant.fence = entl::FenceMask::standard();
  grant.actor = re_test::must_parse<entl::ActorId>("operator-1");
  return grant;
}

std::vector<std::uint8_t> encode_record(entl::Store& store, const EntitlementId& id) {
  entl::CanonicalWriter writer;
  store.get(id).value().encode(writer);
  return writer.data();
}

}  // namespace

RE_TEST(durable, restart_reproduces_every_durable_field_exactly) {
  re_test::TempDirectory holder("restart-exact");
  EntitlementId id;
  std::vector<std::uint8_t> before;
  const entl::Timestamp now = re_test::instant(1700000005);

  {
    auto opened = re_test::open_store_at(holder.path(), true);
    RE_REQUIRE(opened.has_value());
    entl::Store& store = *opened.value();
    RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
        "exact-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 2u)));
    auto outcome = store.grant(simple_grant("exact-grant", re_test::instant(1700000001), 42u));
    RE_REQUIRE(outcome.has_value());
    id = outcome->primary_id;
    before = encode_record(store, id);
    RE_CHECK_EQ(store.commit_seq().value(), std::uint64_t{2});
  }

  auto reopened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(reopened.has_value());
  entl::Store& store = *reopened.value();
  RE_CHECK_EQ(store.commit_seq().value(), std::uint64_t{2});
  const std::vector<std::uint8_t> after = encode_record(store, id);
  RE_CHECK(before == after);
  RE_CHECK_EQ(store.authority().facility_capacity_generation.value(), std::uint64_t{2});
  entl::VerifyRequest verify;
  verify.id = id;
  verify.now = now;
  RE_CHECK(store.verify(verify)->authorized);
}

RE_TEST(durable, many_commits_survive_segment_rotation_and_reopen) {
  re_test::TempDirectory holder("rotation");
  entl::StoreOpenOptions options = re_test::test_options(holder.path(), true);
  options.max_segment_bytes = 4096u;
  options.max_journal_entry_bytes = 2048u;
  std::vector<EntitlementId> ids;
  std::uint64_t sequence = 0;
  {
    auto opened = entl::Store::open(options);
    RE_REQUIRE(opened.has_value());
    entl::Store& store = *opened.value();
    RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
        "rotation-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 1u)));
    for (std::uint64_t index = 0; index < 40u; ++index) {
      auto outcome = store.grant(
          simple_grant("rotation-grant-" + std::to_string(index), re_test::instant(1700000001), 2u));
      RE_REQUIRE(outcome.has_value());
      ids.push_back(outcome->primary_id);
    }
    sequence = store.commit_seq().value();
    RE_CHECK(store.stats().log_segment_count > 1u);
  }
  auto reopened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(reopened.has_value());
  entl::Store& store = *reopened.value();
  RE_CHECK_EQ(store.commit_seq().value(), sequence);
  RE_CHECK_EQ(store.stats().entitlement_count, ids.size());
  for (const EntitlementId& id : ids) {
    RE_CHECK(store.get(id).has_value());
  }
}

RE_TEST(durable, uncommitted_journal_tail_is_discarded_at_open) {
  re_test::TempDirectory holder("torn-tail");
  EntitlementId id;
  {
    auto opened = re_test::open_store_at(holder.path(), true);
    RE_REQUIRE(opened.has_value());
    entl::Store& store = *opened.value();
    RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
        "tail-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 1u)));
    auto outcome = store.grant(simple_grant("tail-grant", re_test::instant(1700000001), 7u));
    RE_REQUIRE(outcome.has_value());
    id = outcome->primary_id;
  }

  // Append bytes that look like the beginning of a record but were never made
  // authoritative by a manifest publication, exactly as an interrupted commit
  // would leave them.
  std::vector<std::uint8_t> garbage;
  for (std::size_t index = 0; index < 200u; ++index) {
    garbage.push_back(static_cast<std::uint8_t>((index * 37u) & 0xFFu));
  }
  const std::filesystem::path target = holder.path() / "log-00000000.bin";
  {
    std::ofstream out(target, std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<const char*>(garbage.data()), static_cast<std::streamsize>(garbage.size()));
  }

  // A read-only inspection must see the uncommitted tail without repairing it.
  auto inspection = entl::Store::inspect(holder.path());
  RE_REQUIRE(inspection.has_value());
  RE_CHECK(inspection->torn_tail_discarded);
  RE_CHECK_EQ(inspection->commit_seq.value(), std::uint64_t{2});
  RE_CHECK_EQ(inspection->entitlements, std::size_t{1});

  auto reopened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(reopened.has_value());
  entl::Store& store = *reopened.value();
  RE_CHECK_EQ(store.commit_seq().value(), std::uint64_t{2});
  RE_CHECK(store.get(id).has_value());
  RE_CHECK_EQ(store.get(id)->remaining.units(), std::uint64_t{7});

  // The writer open repaired the tail, so a second inspection is clean.
  auto repaired = entl::Store::inspect(holder.path());
  RE_REQUIRE(repaired.has_value());
  RE_CHECK(!repaired->torn_tail_discarded);
}

RE_TEST(durable, a_second_writer_is_refused_while_the_first_holds_the_lock) {
  re_test::TempDirectory holder("writer-lock");
  auto first = re_test::open_store_at(holder.path(), true);
  RE_REQUIRE(first.has_value());
  auto second = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(!second.has_value());
  RE_CHECK_EQ(second.error().code(), ErrorCode::kWriterLockHeld);

  // Read-only inspection does not take the writer lock and still works.
  auto inspection = entl::Store::inspect(holder.path());
  RE_REQUIRE(inspection.has_value());
  RE_CHECK(!inspection->writer_lock_free);

  entl::StoreOpenOptions read_only = re_test::test_options(holder.path(), false);
  read_only.read_only = true;
  auto reader = entl::Store::open(read_only);
  RE_REQUIRE(reader.has_value());
  RE_CHECK(reader.value()->is_read_only());
}

RE_TEST(durable, a_restored_older_manifest_is_detected_as_a_rollback) {
  re_test::TempDirectory holder("rollback");
  std::vector<std::uint8_t> manifest_a_backup;
  std::vector<std::uint8_t> manifest_b_backup;
  {
    auto opened = re_test::open_store_at(holder.path(), true);
    RE_REQUIRE(opened.has_value());
    entl::Store& store = *opened.value();
    RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
        "rollback-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 1u)));
    RE_REQUIRE_OK(store.grant(simple_grant("rollback-grant-a", re_test::instant(1700000001), 5u)));
    manifest_a_backup = re_test::read_bytes(holder.path() / "manifest.a");
    manifest_b_backup = re_test::read_bytes(holder.path() / "manifest.b");
    for (std::uint64_t index = 0; index < 6u; ++index) {
      RE_REQUIRE_OK(store.grant(
          simple_grant("rollback-grant-" + std::to_string(index), re_test::instant(1700000002), 5u)));
    }
    RE_CHECK(store.commit_seq().value() > 6u);
  }

  // Restore the whole directory to the older consistent pair of manifests. The
  // fence high-water mark, which is advanced after every publication, still
  // records the newer sequence.
  re_test::write_bytes(holder.path() / "manifest.a", manifest_a_backup);
  re_test::write_bytes(holder.path() / "manifest.b", manifest_b_backup);

  auto reopened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(!reopened.has_value());
  RE_CHECK_EQ(reopened.error().code(), ErrorCode::kRollbackDetected);

  auto inspection = entl::Store::inspect(holder.path());
  RE_REQUIRE(!inspection.has_value());
  RE_CHECK_EQ(inspection.error().code(), ErrorCode::kRollbackDetected);
}

RE_TEST(durable, a_missing_fence_degrades_rollback_detection_but_still_opens) {
  re_test::TempDirectory holder("fence-missing");
  {
    auto opened = re_test::open_store_at(holder.path(), true);
    RE_REQUIRE(opened.has_value());
    entl::Store& store = *opened.value();
    RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
        "fence-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 1u)));
    RE_REQUIRE_OK(store.grant(simple_grant("fence-grant", re_test::instant(1700000001), 3u)));
  }
  std::error_code ec;
  std::filesystem::remove(holder.path() / "fence.bin", ec);
  RE_CHECK(!ec);

  auto reopened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(reopened.has_value());
  RE_CHECK_EQ(reopened.value()->commit_seq().value(), std::uint64_t{2});
  RE_CHECK(std::filesystem::exists(holder.path() / "fence.bin"));
}

RE_TEST(durable, read_only_open_performs_the_same_integrity_checks) {
  re_test::TempDirectory holder("read-only-integrity");
  {
    auto opened = re_test::open_store_at(holder.path(), true);
    RE_REQUIRE(opened.has_value());
    entl::Store& store = *opened.value();
    RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
        "ro-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 1u)));
    RE_REQUIRE_OK(store.grant(simple_grant("ro-grant", re_test::instant(1700000001), 3u)));
  }
  // Corrupt every manifest slot; a read-only open must refuse rather than fall
  // back to an empty state.
  std::vector<std::uint8_t> bytes = re_test::read_bytes(holder.path() / "manifest.a");
  bytes[40] = static_cast<std::uint8_t>(bytes[40] ^ 0xFFu);
  re_test::write_bytes(holder.path() / "manifest.a", bytes);
  bytes = re_test::read_bytes(holder.path() / "manifest.b");
  bytes[40] = static_cast<std::uint8_t>(bytes[40] ^ 0xFFu);
  re_test::write_bytes(holder.path() / "manifest.b", bytes);

  entl::StoreOpenOptions read_only = re_test::test_options(holder.path(), false);
  read_only.read_only = true;
  auto opened = entl::Store::open(read_only);
  RE_REQUIRE(!opened.has_value());
  RE_CHECK_EQ(opened.error().code(), ErrorCode::kCorruptManifest);
}

RE_TEST(durable, opening_a_directory_with_no_store_reports_store_not_found) {
  re_test::TempDirectory holder("empty");
  auto opened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(!opened.has_value());
  RE_CHECK_EQ(opened.error().code(), ErrorCode::kStoreNotFound);

  auto created = re_test::open_store_at(holder.path(), true);
  RE_REQUIRE(created.has_value());
  RE_CHECK(created.value()->commit_seq().is_zero());
}
