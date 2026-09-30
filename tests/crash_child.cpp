// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Test helper: opens a store, begins one mutation, and terminates the process
// abruptly at a chosen durable commit stage. Never installed.

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "resource_entitlement/store.hpp"

namespace {

void die_at_stage(const std::string& wanted) {
  // std::_Exit terminates without unwinding, without running static destructors,
  // and without raising any interactive crash dialog.
  std::cout << "terminating at stage " << wanted << std::endl;
  std::cout.flush();
  std::_Exit(137);
}

entl::GrantRequest build_grant(const std::string& seed) {
  entl::GrantRequest grant;
  grant.request_id = entl::RequestId::derive(seed);
  grant.now = entl::Timestamp::parse("2026-02-01T00:00:00Z").value();
  grant.holder = entl::TenantId::parse("tenant-a").value();
  grant.service = entl::ServiceId::parse("inference").value();
  grant.service_class = entl::ServiceClassId::parse("gold").value();
  grant.scope.facility = entl::FacilityId::parse("dc-1").value();
  grant.scope.resource_type = entl::ResourceTypeId::parse("accelerator").value();
  grant.scope.scope = entl::ResourceScopeId::parse("scope-a").value();
  grant.unit = entl::Unit::kCount;
  grant.quantity = entl::Quantity::make(entl::Unit::kCount, 5u).value();
  grant.effective_from = entl::Timestamp::parse("2026-01-01T00:00:00Z").value();
  grant.expires_at = entl::Timestamp::parse("2027-01-01T00:00:00Z").value();
  grant.admission_decision_digest = entl::Sha256::hash("crash-admission");
  grant.fence = entl::FenceMask::standard();
  grant.actor = entl::ActorId::parse("operator-1").value();
  return grant;
}

}  // namespace

int main(int argc, char** argv) {
  std::string directory;
  std::string stage;
  std::string seed = "crash-grant";
  std::uint64_t max_segment_bytes = 4u * 1024u * 1024u;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto value_of = [&](const char* flag) {
      return argument.rfind(flag, 0) == 0 ? argument.substr(std::strlen(flag)) : std::string();
    };
    std::string value = value_of("--dir=");
    if (!value.empty()) {
      directory = value;
      continue;
    }
    value = value_of("--stage=");
    if (!value.empty()) {
      stage = value;
      continue;
    }
    value = value_of("--seed=");
    if (!value.empty()) {
      seed = value;
      continue;
    }
    value = value_of("--max-segment-bytes=");
    if (!value.empty()) {
      max_segment_bytes = std::strtoull(value.c_str(), nullptr, 10);
      continue;
    }
  }
  if (directory.empty()) {
    std::cerr << "usage: --dir=PATH [--stage=NAME] [--seed=TEXT] [--max-segment-bytes=N]" << std::endl;
    return 2;
  }

  entl::StoreOpenOptions options;
  options.directory = std::filesystem::path(directory);
  options.max_segment_bytes = static_cast<std::uint32_t>(max_segment_bytes);
  if (max_segment_bytes >= 2048u) {
    options.max_journal_entry_bytes = static_cast<std::uint32_t>(max_segment_bytes / 2u);
  }
  if (!stage.empty()) {
    options.commit_stage_observer = [&stage](entl::CommitStage observed) {
      if (stage == entl::commit_stage_name(observed)) {
        die_at_stage(stage);
      }
    };
  }

  auto opened = entl::Store::open(options);
  if (!opened.has_value()) {
    std::cerr << "open failed: " << opened.error().to_string() << std::endl;
    return 3;
  }
  auto outcome = opened.value()->grant(build_grant(seed));
  if (!outcome.has_value()) {
    std::cerr << "grant refused: " << outcome.error().to_string() << std::endl;
    return 4;
  }
  std::cout << "committed " << outcome->primary_id.to_hex() << " at " << outcome->commit_seq.value()
            << std::endl;
  return 0;
}
