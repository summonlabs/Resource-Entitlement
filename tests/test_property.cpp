// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Seeded randomized state-machine tests. Every seed is printed so a failure is
// reproducible from the log alone.

#include <algorithm>
#include <iostream>
#include <cstdint>
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

/// Deterministic splitmix64 generator. Property tests must be reproducible.
class Rng {
public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
  }

  std::uint64_t below(std::uint64_t bound) noexcept { return bound == 0u ? 0u : next() % bound; }

  bool chance(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    return below(denominator) < numerator;
  }

private:
  std::uint64_t state_;
};

struct Machine {
  explicit Machine(std::uint64_t seed)
      : rng(seed), directory("property"), store(nullptr), counter(0u), time(1700000000) {}

  Rng rng;
  re_test::TempDirectory directory;
  std::unique_ptr<entl::Store> store;
  std::vector<EntitlementId> ids;
  std::uint64_t counter;
  std::int64_t time;

  entl::Timestamp now() { return re_test::instant(time); }

  void advance() {
    ++counter;
    ++time;
  }

  std::string next_seed(std::string_view prefix) {
    advance();
    return std::string(prefix) + "-" + std::to_string(counter);
  }

  std::optional<EntitlementId> pick() {
    if (ids.empty()) {
      return std::nullopt;
    }
    const EntitlementId candidate = ids[rng.below(ids.size())];
    if (!store->get(candidate).has_value()) {
      return std::nullopt;
    }
    return candidate;
  }

  bool open() {
    auto opened = re_test::open_store_at(directory.path(), false);
    if (!opened.has_value()) {
      return false;
    }
    store = std::move(opened.value());
    return true;
  }
};

/// Invariants that must hold after every single action.
void check_invariants(Machine& machine, const char* action) {
  entl::ListFilter filter;
  filter.limit = 65536u;
  auto listed = machine.store->list(filter);
  if (!listed.has_value()) {
    RE_FAIL(std::string("list failed after ") + action + ": " + listed.error().to_string());
    return;
  }
  const std::uint64_t sequence = machine.store->commit_seq().value();
  for (const entl::Entitlement& record : listed.value()) {
    if (record.remaining.units() > record.granted.units()) {
      RE_FAIL(std::string("remaining exceeds granted after ") + action);
    }
    if (record.delegated_out.units() > record.granted.units()) {
      RE_FAIL(std::string("delegated_out exceeds granted after ") + action);
    }
    if (record.revision.is_zero()) {
      RE_FAIL(std::string("zero revision after ") + action);
    }
    if (!(record.effective_from < record.expires_at)) {
      RE_FAIL(std::string("empty validity window after ") + action);
    }
    if (record.last_commit_seq.value() > sequence) {
      RE_FAIL(std::string("record cites a future commit after ") + action);
    }
    if (entl::is_terminal_state(record.state) && record.remaining.is_zero() == false) {
      // Terminal records keep their historical accounting; authority is removed
      // by the state, so this is legal, and verification must still refuse.
      entl::VerifyRequest verify;
      verify.id = record.id;
      verify.now = machine.now();
      auto decision = machine.store->verify(verify);
      if (!decision.has_value() || decision->authorized) {
        RE_FAIL(std::string("a terminal record authorized use after ") + action);
      }
    }
  }
  if (listed.value().size() != machine.store->stats().entitlement_count) {
    RE_FAIL(std::string("list and stats disagree after ") + action);
  }
}

void action_grant(Machine& machine) {
  machine.advance();
  entl::GrantRequest grant;
  grant.request_id = entl::RequestId::derive(machine.next_seed("grant"));
  grant.now = machine.now();
  grant.holder = re_test::must_parse<entl::TenantId>(
      std::string("tenant-") + std::to_string(machine.rng.below(3u)));
  grant.service = re_test::must_parse<entl::ServiceId>("inference");
  grant.service_class = re_test::must_parse<entl::ServiceClassId>("gold");
  grant.scope.facility = re_test::must_parse<entl::FacilityId>("dc-1");
  grant.scope.resource_type = re_test::must_parse<entl::ResourceTypeId>("accelerator");
  grant.scope.scope = re_test::must_parse<entl::ResourceScopeId>(
      std::string("scope-") + std::to_string(machine.rng.below(2u)));
  grant.unit = Unit::kCount;
  grant.quantity = entl::Quantity::make(Unit::kCount, 1u + machine.rng.below(50u)).value();
  grant.priority.klass = static_cast<entl::PriorityClass>(
      1u + machine.rng.below(6u));
  grant.priority.rank = static_cast<std::uint16_t>(machine.rng.below(4u));
  grant.effective_from = re_test::instant(machine.time - 100);
  grant.expires_at = re_test::instant(machine.time + 100000 + static_cast<std::int64_t>(machine.rng.below(1000u)));
  grant.admission_decision_digest = re_test::digest_of(grant.request_id.to_hex());
  grant.fence = (machine.rng.below(4u) == 0u) ? entl::FenceMask::control_epoch_only()
                                              : entl::FenceMask::standard();
  grant.actor = re_test::must_parse<entl::ActorId>("operator-1");
  grant.note = machine.rng.below(4u) == 0u ? std::string("granted by the property machine") : std::string();
  auto outcome = machine.store->grant(grant);
  if (outcome.has_value()) {
    machine.ids.push_back(outcome->primary_id);
  }
}

void action_draw(Machine& machine) {
  auto id = machine.pick();
  if (!id.has_value()) {
    return;
  }
  auto record = machine.store->get(id.value());
  if (!record.has_value()) {
    return;
  }
  machine.advance();
  entl::DrawRequest draw;
  draw.request_id = entl::RequestId::derive(machine.next_seed("draw"));
  draw.now = machine.now();
  draw.id = id.value();
  draw.expected_revision = record->revision;
  draw.quantity = entl::Quantity::make(Unit::kCount, 1u + machine.rng.below(20u)).value();
  draw.actor = re_test::must_parse<entl::ActorId>("operator-1");
  (void)machine.store->draw(draw);
}

void action_release(Machine& machine) {
  auto id = machine.pick();
  if (!id.has_value()) {
    return;
  }
  auto record = machine.store->get(id.value());
  if (!record.has_value()) {
    return;
  }
  machine.advance();
  entl::ReleaseRequest release;
  release.request_id = entl::RequestId::derive(machine.next_seed("release"));
  release.now = machine.now();
  release.id = id.value();
  release.expected_revision = record->revision;
  release.quantity = entl::Quantity::make(Unit::kCount, 1u + machine.rng.below(10u)).value();
  release.actor = re_test::must_parse<entl::ActorId>("operator-1");
  (void)machine.store->release(release);
}

void action_suspend_resume(Machine& machine) {
  auto id = machine.pick();
  if (!id.has_value()) {
    return;
  }
  auto record = machine.store->get(id.value());
  if (!record.has_value()) {
    return;
  }
  machine.advance();
  if (record->state == entl::EntitlementState::kActive) {
    entl::SuspendRequest suspend;
    suspend.request_id = entl::RequestId::derive(machine.next_seed("suspend"));
    suspend.now = machine.now();
    suspend.id = id.value();
    suspend.expected_revision = record->revision;
    suspend.actor = re_test::must_parse<entl::ActorId>("operator-1");
    (void)machine.store->suspend(suspend);
  } else if (record->state == entl::EntitlementState::kSuspended) {
    entl::ResumeRequest resume;
    resume.request_id = entl::RequestId::derive(machine.next_seed("resume"));
    resume.now = machine.now();
    resume.id = id.value();
    resume.expected_revision = record->revision;
    resume.actor = re_test::must_parse<entl::ActorId>("operator-1");
    (void)machine.store->resume(resume);
  }
}

void action_revoke(Machine& machine) {
  auto id = machine.pick();
  if (!id.has_value()) {
    return;
  }
  auto record = machine.store->get(id.value());
  if (!record.has_value()) {
    return;
  }
  machine.advance();
  entl::RevokeRequest revoke;
  revoke.request_id = entl::RequestId::derive(machine.next_seed("revoke"));
  revoke.now = machine.now();
  revoke.id = id.value();
  revoke.expected_revision = record->revision;
  revoke.reason = machine.rng.chance(1u, 2u) ? entl::TerminalReason::kOperatorRevocation
                                             : entl::TerminalReason::kPolicyRevocation;
  revoke.cascade_delegated_children = machine.rng.chance(1u, 2u);
  revoke.actor = re_test::must_parse<entl::ActorId>("operator-1");
  (void)machine.store->revoke(revoke);
}

void action_expire(Machine& machine) {
  auto id = machine.pick();
  if (!id.has_value()) {
    return;
  }
  auto record = machine.store->get(id.value());
  if (!record.has_value()) {
    return;
  }
  machine.advance();
  entl::ExpireRequest expire;
  expire.request_id = entl::RequestId::derive(machine.next_seed("expire"));
  expire.now = machine.now();
  expire.id = id.value();
  expire.expected_revision = record->revision;
  expire.actor = re_test::must_parse<entl::ActorId>("operator-1");
  (void)machine.store->expire(expire);
}

void action_transfer(Machine& machine) {
  auto id = machine.pick();
  if (!id.has_value()) {
    return;
  }
  auto record = machine.store->get(id.value());
  if (!record.has_value()) {
    return;
  }
  machine.advance();
  entl::TransferRequest transfer;
  transfer.request_id = entl::RequestId::derive(machine.next_seed("transfer"));
  transfer.now = machine.now();
  transfer.source = id.value();
  transfer.expected_revision = record->revision;
  const std::uint64_t choice = machine.rng.below(3u);
  transfer.mode = choice == 0u   ? entl::TransferMode::kMove
                  : choice == 1u ? entl::TransferMode::kSplit
                                 : entl::TransferMode::kDelegate;
  transfer.target_holder = re_test::must_parse<entl::TenantId>(
      std::string("tenant-") + std::to_string(machine.rng.below(4u)));
  if (transfer.mode != entl::TransferMode::kMove) {
    transfer.quantity =
        entl::Quantity::make(Unit::kCount, 1u + machine.rng.below(record->remaining.units() + 1u)).value();
  }
  transfer.priority = record->priority;
  if (machine.rng.chance(1u, 3u) && transfer.priority.klass != entl::PriorityClass::kBackground) {
    transfer.priority.klass = static_cast<entl::PriorityClass>(
        static_cast<std::uint8_t>(transfer.priority.klass) - 1u);
  }
  transfer.actor = re_test::must_parse<entl::ActorId>("operator-1");
  auto outcome = machine.store->transfer(transfer);
  if (outcome.has_value() && transfer.mode != entl::TransferMode::kMove) {
    machine.ids.push_back(outcome->primary_id);
  }
}

void action_merge(Machine& machine) {
  if (machine.ids.size() < 2u) {
    return;
  }
  const EntitlementId first = machine.ids[machine.rng.below(machine.ids.size())];
  const EntitlementId second = machine.ids[machine.rng.below(machine.ids.size())];
  if (first == second) {
    return;
  }
  auto first_record = machine.store->get(first);
  auto second_record = machine.store->get(second);
  if (!first_record.has_value() || !second_record.has_value()) {
    return;
  }
  machine.advance();
  entl::MergeRequest merge;
  merge.request_id = entl::RequestId::derive(machine.next_seed("merge"));
  merge.now = machine.now();
  merge.first = first;
  merge.first_expected_revision = first_record->revision;
  merge.second = second;
  merge.second_expected_revision = second_record->revision;
  merge.actor = re_test::must_parse<entl::ActorId>("operator-1");
  auto outcome = machine.store->merge(merge);
  if (outcome.has_value()) {
    machine.ids.push_back(outcome->primary_id);
  }
}

void action_reissue(Machine& machine) {
  auto id = machine.pick();
  if (!id.has_value()) {
    return;
  }
  auto record = machine.store->get(id.value());
  if (!record.has_value()) {
    return;
  }
  machine.advance();
  entl::ReissueRequest reissue;
  reissue.request_id = entl::RequestId::derive(machine.next_seed("reissue"));
  reissue.now = machine.now();
  reissue.id = id.value();
  reissue.expected_revision = record->revision;
  reissue.rebind_to_current_authority = machine.rng.chance(3u, 4u);
  reissue.actor = re_test::must_parse<entl::ActorId>("operator-1");
  auto outcome = machine.store->reissue(reissue);
  if (outcome.has_value()) {
    machine.ids.push_back(outcome->primary_id);
  }
}

void action_authority(Machine& machine) {
  machine.advance();
  entl::AuthorityUpdateRequest update = re_test::authority_request(
      machine.next_seed("authority"), machine.now(), machine.store->authority().revision,
      machine.rng.below(20u) + 1u);
  if (machine.rng.chance(1u, 6u)) {
    update.advance_epoch = true;
  }
  (void)machine.store->update_authority(update);
}

void run_seed(std::uint64_t seed, int actions) {
  Machine machine(seed);
  {
    auto opened = re_test::open_store_at(machine.directory.path(), true);
    RE_REQUIRE(opened.has_value());
    machine.store = std::move(opened.value());
  }
  RE_REQUIRE_OK(machine.store->update_authority(re_test::authority_request(
      "property-authority", machine.now(), entl::Revision::from_value(1u), 1u)));

  for (int step = 0; step < actions; ++step) {
    const std::uint64_t choice = machine.rng.below(100u);
    const char* action = "unknown";
    if (choice < 24u) {
      action = "grant";
      action_grant(machine);
    } else if (choice < 40u) {
      action = "draw";
      action_draw(machine);
    } else if (choice < 50u) {
      action = "release";
      action_release(machine);
    } else if (choice < 58u) {
      action = "suspend_or_resume";
      action_suspend_resume(machine);
    } else if (choice < 66u) {
      action = "revoke";
      action_revoke(machine);
    } else if (choice < 72u) {
      action = "expire";
      action_expire(machine);
    } else if (choice < 84u) {
      action = "transfer";
      action_transfer(machine);
    } else if (choice < 88u) {
      action = "merge";
      action_merge(machine);
    } else if (choice < 94u) {
      action = "reissue";
      action_reissue(machine);
    } else if (choice < 99u) {
      action = "authority";
      action_authority(machine);
    } else {
      action = "compact";
      machine.advance();
      (void)machine.store->compact();
    }
    check_invariants(machine, action);
  }

  // Restart proof: the recovered state must be exactly the state that was
  // committed, byte for byte, for every surviving record.
  std::vector<std::string> before;
  entl::ListFilter filter;
  filter.limit = 65536u;
  {
    auto listed = machine.store->list(filter);
    RE_REQUIRE(listed.has_value());
    for (const entl::Entitlement& record : listed.value()) {
      entl::CanonicalWriter writer;
      record.encode(writer);
      before.push_back(writer.to_hex());
    }
  }
  const std::uint64_t sequence = machine.store->commit_seq().value();
  machine.store.reset();
  RE_REQUIRE(machine.open());
  RE_CHECK_EQ(machine.store->commit_seq().value(), sequence);
  std::vector<std::string> after;
  {
    auto listed = machine.store->list(filter);
    RE_REQUIRE(listed.has_value());
    for (const entl::Entitlement& record : listed.value()) {
      entl::CanonicalWriter writer;
      record.encode(writer);
      after.push_back(writer.to_hex());
    }
  }
  RE_CHECK(before == after);
  check_invariants(machine, "restart");
}

}  // namespace

RE_TEST(property, randomized_state_machine_seed_1) {
  std::cout << "    property seed 1" << std::endl;
  run_seed(1u, 300);
}

RE_TEST(property, randomized_state_machine_seed_2) {
  std::cout << "    property seed 2" << std::endl;
  run_seed(2u, 300);
}

RE_TEST(property, randomized_state_machine_seed_3) {
  std::cout << "    property seed 3" << std::endl;
  run_seed(3u, 300);
}

RE_TEST(property, randomized_state_machine_seed_20260214) {
  std::cout << "    property seed 20260214" << std::endl;
  run_seed(20260214u, 400);
}

RE_TEST(property, randomized_state_machine_seed_99991) {
  std::cout << "    property seed 99991" << std::endl;
  run_seed(99991u, 250);
}
