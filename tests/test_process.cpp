// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Real independent operating-system process tests: writer-lock exclusion,
// kernel lock release after abrupt death, and crash consistency at every
// durable commit stage.

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "harness.hpp"
#include "resource_entitlement/store.hpp"
#include "test_support.hpp"

using entl::ErrorCode;
using entl::Unit;

namespace {

entl::GrantRequest build_grant(std::string_view seed) {
  entl::GrantRequest grant;
  grant.request_id = re_test::request_id(seed);
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
  grant.admission_decision_digest = re_test::digest_of(std::string("admission-") + std::string(seed));
  grant.fence = entl::FenceMask::standard();
  grant.actor = re_test::must_parse<entl::ActorId>("operator-1");
  return grant;
}

void seed_store(const std::filesystem::path& directory, std::uint64_t grants, std::uint64_t segment_bytes) {
  entl::StoreOpenOptions options = re_test::test_options(directory, true);
  options.max_segment_bytes = static_cast<std::uint32_t>(segment_bytes);
  options.max_journal_entry_bytes = static_cast<std::uint32_t>(segment_bytes / 2u);
  auto opened = entl::Store::open(options);
  if (!opened.has_value()) {
    throw std::runtime_error("seed_store: " + opened.error().to_string());
  }
  entl::Store& store = *opened.value();
  auto authority = store.update_authority(re_test::authority_request(
      "process-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 1u));
  if (!authority.has_value()) {
    throw std::runtime_error("seed_store authority: " + authority.error().to_string());
  }
  for (std::uint64_t index = 0; index < grants; ++index) {
    auto outcome = store.grant(build_grant("process-grant-" + std::to_string(index)));
    if (!outcome.has_value()) {
      throw std::runtime_error("seed_store grant: " + outcome.error().to_string());
    }
  }
}

}  // namespace

RE_TEST(process, writer_lock_is_held_by_a_real_second_process) {
  const std::filesystem::path holder_child = re_test::executable_from_env("ENTL_HOLDER_CHILD");
  RE_REQUIRE(!holder_child.empty());
  re_test::TempDirectory holder("lock-cross");
  seed_store(holder.path(), 0u, 4u * 1024u * 1024u);

  const std::filesystem::path ready = holder.path() / "ready.txt";
  auto child = re_test::ChildProcess::start(
      holder_child, {"--dir=" + holder.path().string(), "--ready=" + ready.string(), "--hold-millis=60000"});
  RE_REQUIRE(child.has_value());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!std::filesystem::exists(ready) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  RE_REQUIRE(std::filesystem::exists(ready));

  // The parent cannot take the writer lock while the child holds it.
  auto blocked = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(!blocked.has_value());
  RE_CHECK_EQ(blocked.error().code(), ErrorCode::kWriterLockHeld);

  // The parent can still inspect read-only.
  auto inspection = entl::Store::inspect(holder.path());
  RE_REQUIRE(inspection.has_value());
  RE_CHECK(!inspection->writer_lock_free);

  // Killing the child must release the kernel lock without any cleanup.
  child->terminate_now();
  const re_test::ProcessResult result = child->wait();
  (void)result;
  RE_CHECK(!child->running());

  const auto release_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  bool acquired = false;
  while (!acquired && std::chrono::steady_clock::now() < release_deadline) {
    auto attempt = re_test::open_store_at(holder.path(), false);
    if (attempt.has_value()) {
      acquired = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  RE_CHECK(acquired);
}

RE_TEST(process, abrupt_death_at_every_commit_stage_leaves_a_consistent_store) {
  const std::filesystem::path crash_child = re_test::executable_from_env("ENTL_CRASH_CHILD");
  RE_REQUIRE(!crash_child.empty());

  const char* stages[] = {"before_append",          "after_append_before_flush",
                          "after_flush_before_manifest", "after_manifest_publish",
                          "after_segment_rotate_before_manifest"};

  for (const char* stage : stages) {
    re_test::TempDirectory holder("crash-stage");
    seed_store(holder.path(), 4u, 4096u);
    std::uint64_t before_sequence = 0;
    std::size_t before_count = 0;
    {
      auto opened = re_test::open_store_at(holder.path(), false);
      RE_REQUIRE(opened.has_value());
      before_sequence = opened.value()->commit_seq().value();
      before_count = opened.value()->stats().entitlement_count;
    }

    const re_test::ProcessResult result = re_test::run_process(
        crash_child, {"--dir=" + holder.path().string(), std::string("--stage=") + stage,
                      std::string("--seed=crash-") + stage, "--max-segment-bytes=4096"});

    auto reopened = re_test::open_store_at(holder.path(), false);
    RE_REQUIRE(reopened.has_value());
    entl::Store& store = *reopened.value();
    const std::uint64_t after_sequence = store.commit_seq().value();
    const std::size_t after_count = store.stats().entitlement_count;

    RE_CHECK(after_sequence == before_sequence || after_sequence == before_sequence + 1u);
    if (after_sequence == before_sequence) {
      RE_CHECK_EQ(after_count, before_count);
      RE_CHECK(!result.started || result.exit_code != 0);
    } else {
      RE_CHECK_EQ(after_count, before_count + 1u);
    }

    // Whatever happened, the recovered state must satisfy every invariant and
    // the store must still accept work.
    auto inspection = entl::Store::inspect(holder.path());
    RE_REQUIRE(inspection.has_value());
    RE_CHECK_EQ(inspection->commit_seq.value(), after_sequence);
    RE_CHECK_EQ(inspection->entitlements, after_count);

    auto outcome = store.grant(build_grant(std::string("post-crash-") + stage));
    RE_REQUIRE(outcome.has_value());
    RE_CHECK_EQ(store.commit_seq().value(), after_sequence + 1u);
  }
}

RE_TEST(process, repeated_crash_and_restart_cycles_never_lose_committed_work) {
  const std::filesystem::path crash_child = re_test::executable_from_env("ENTL_CRASH_CHILD");
  RE_REQUIRE(!crash_child.empty());
  re_test::TempDirectory holder("crash-cycles");
  seed_store(holder.path(), 1u, 4u * 1024u * 1024u);

  std::uint64_t expected_sequence = 0;
  std::size_t expected_count = 0;
  {
    auto opened = re_test::open_store_at(holder.path(), false);
    RE_REQUIRE(opened.has_value());
    expected_sequence = opened.value()->commit_seq().value();
    expected_count = opened.value()->stats().entitlement_count;
  }

  for (int cycle = 0; cycle < 6; ++cycle) {
    const std::string seed = "cycle-" + std::to_string(cycle);
    const re_test::ProcessResult result = re_test::run_process(
        crash_child, {"--dir=" + holder.path().string(), "--stage=after_flush_before_manifest",
                      "--seed=" + seed});
    (void)result;
    auto reopened = re_test::open_store_at(holder.path(), false);
    RE_REQUIRE(reopened.has_value());
    entl::Store& store = *reopened.value();
    // A crash before the manifest publication must never be visible.
    RE_CHECK_EQ(store.commit_seq().value(), expected_sequence);
    RE_CHECK_EQ(store.stats().entitlement_count, expected_count);

    auto outcome = store.grant(build_grant("completion-" + std::to_string(cycle)));
    RE_REQUIRE(outcome.has_value());
    expected_sequence += 1u;
    expected_count += 1u;
  }

  auto reopened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(reopened.has_value());
  RE_CHECK_EQ(reopened.value()->commit_seq().value(), expected_sequence);
  RE_CHECK_EQ(reopened.value()->stats().entitlement_count, expected_count);
}
