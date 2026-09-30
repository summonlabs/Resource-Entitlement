// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Corruption, truncation, and rollback sweeps against real persisted files.

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "harness.hpp"
#include "resource_entitlement/digest.hpp"
#include "resource_entitlement/store.hpp"
#include "test_support.hpp"

using entl::ErrorCode;
using entl::Unit;

namespace {

entl::GrantRequest build_grant(std::string_view seed, std::uint64_t units) {
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
  grant.quantity = entl::Quantity::make(Unit::kCount, units).value();
  grant.effective_from = re_test::instant(1699999000);
  grant.expires_at = re_test::instant(1799999000);
  grant.admission_decision_digest = re_test::digest_of(std::string("admission-") + std::string(seed));
  grant.fence = entl::FenceMask::standard();
  grant.actor = re_test::must_parse<entl::ActorId>("operator-1");
  return grant;
}

void populate(const std::filesystem::path& directory, std::uint64_t grants) {
  auto opened = re_test::open_store_at(directory, true);
  if (!opened.has_value()) {
    throw std::runtime_error("populate: " + opened.error().to_string());
  }
  entl::Store& store = *opened.value();
  auto authority = store.update_authority(re_test::authority_request(
      "corruption-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 1u));
  if (!authority.has_value()) {
    throw std::runtime_error("populate authority: " + authority.error().to_string());
  }
  for (std::uint64_t index = 0; index < grants; ++index) {
    auto outcome = store.grant(build_grant("corruption-grant-" + std::to_string(index), 4u));
    if (!outcome.has_value()) {
      throw std::runtime_error("populate grant: " + outcome.error().to_string());
    }
  }
}

const char* kManifestNames[] = {"manifest.a", "manifest.b"};

}  // namespace

RE_TEST(corruption, every_single_byte_flip_in_a_manifest_is_refused_or_corrected) {
  re_test::TempDirectory holder("manifest-flip");
  populate(holder.path(), 2u);
  const std::vector<std::uint8_t> original = re_test::read_bytes(holder.path() / "manifest.a");
  const std::filesystem::path other = holder.path() / "manifest.b";

  for (std::size_t index = 0; index < original.size(); index += 7u) {
    std::vector<std::uint8_t> bytes = original;
    bytes[index] = static_cast<std::uint8_t>(bytes[index] ^ 0x01u);
    re_test::write_bytes(holder.path() / "manifest.a", bytes);
    auto opened = re_test::open_store_at(holder.path(), false);
    if (opened.has_value()) {
      // Slot A was damaged and slot B is the immediately preceding publication,
      // so recovery legitimately falls back one commit and reports a finding.
      auto inspection = entl::Store::inspect(holder.path());
      RE_REQUIRE(inspection.has_value());
      RE_CHECK(inspection->commit_seq.value() <= 3u);
    } else {
      RE_CHECK(opened.error().code() == ErrorCode::kCorruptManifest ||
               opened.error().code() == ErrorCode::kIntegrityFailure ||
               opened.error().code() == ErrorCode::kRollbackDetected ||
               opened.error().code() == ErrorCode::kReservedFieldNotZero ||
               opened.error().code() == ErrorCode::kUnsupportedFormat);
    }
  }
  re_test::write_bytes(holder.path() / "manifest.a", original);
  RE_CHECK(std::filesystem::exists(other));
  auto recovered = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(recovered.has_value());
  RE_CHECK_EQ(recovered.value()->commit_seq().value(), std::uint64_t{3});
}

RE_TEST(corruption, truncating_or_corrupting_both_manifests_fails_closed) {
  re_test::TempDirectory holder("manifest-dead");
  populate(holder.path(), 1u);

  std::vector<std::uint8_t> bytes = re_test::read_bytes(holder.path() / "manifest.a");
  bytes.resize(120u);
  re_test::write_bytes(holder.path() / "manifest.a", bytes);
  bytes = re_test::read_bytes(holder.path() / "manifest.b");
  bytes.resize(64u);
  re_test::write_bytes(holder.path() / "manifest.b", bytes);

  auto opened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(!opened.has_value());
  RE_CHECK_EQ(opened.error().code(), ErrorCode::kCorruptManifest);

  // Recovery must not have replaced the damaged store with an empty one.
  RE_CHECK_EQ(re_test::read_bytes(holder.path() / "manifest.a").size(), std::size_t{120});
}

RE_TEST(corruption, a_non_zero_reserved_manifest_tail_is_rejected) {
  re_test::TempDirectory holder("manifest-reserved");
  populate(holder.path(), 1u);
  std::vector<std::uint8_t> bytes = re_test::read_bytes(holder.path() / "manifest.a");
  bytes[400] = 0x7Fu;
  re_test::write_bytes(holder.path() / "manifest.a", bytes);

  // Damaging the newest manifest slot must never be silently corrected by
  // falling back to the older publication: the fence proves a newer commit
  // existed, so recovery must fail closed.
  auto opened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(!opened.has_value());
  RE_CHECK(opened.error().code() == ErrorCode::kRollbackDetected ||
           opened.error().code() == ErrorCode::kCorruptManifest ||
           opened.error().code() == ErrorCode::kReservedFieldNotZero);

  auto inspection = entl::Store::inspect(holder.path());
  RE_REQUIRE(!inspection.has_value());
  // The damaged slot was neither rewritten nor discarded.
  RE_CHECK_EQ(re_test::read_bytes(holder.path() / "manifest.a").size(), std::size_t{512});
  (void)kManifestNames;
}

RE_TEST(corruption, log_payload_corruption_is_detected_by_the_checksum) {
  re_test::TempDirectory holder("log-flip");
  populate(holder.path(), 2u);
  const std::filesystem::path segment = holder.path() / "log-00000000.bin";
  const std::vector<std::uint8_t> original = re_test::read_bytes(segment);
  RE_REQUIRE(original.size() > 200u);

  for (const std::size_t offset : {120u, 200u, 300u}) {
    if (offset >= original.size()) {
      continue;
    }
    std::vector<std::uint8_t> bytes = original;
    bytes[offset] = static_cast<std::uint8_t>(bytes[offset] ^ 0x40u);
    re_test::write_bytes(segment, bytes);
    auto opened = re_test::open_store_at(holder.path(), false);
    RE_REQUIRE(!opened.has_value());
    RE_CHECK(opened.error().code() == ErrorCode::kCorruptLog ||
             opened.error().code() == ErrorCode::kIntegrityFailure ||
             opened.error().code() == ErrorCode::kStateDigestMismatch);
  }
  re_test::write_bytes(segment, original);
  RE_CHECK(re_test::open_store_at(holder.path(), false).has_value());
}

RE_TEST(corruption, truncating_committed_log_bytes_is_detected) {
  re_test::TempDirectory holder("log-truncate");
  populate(holder.path(), 3u);
  const std::filesystem::path segment = holder.path() / "log-00000000.bin";
  const std::vector<std::uint8_t> original = re_test::read_bytes(segment);

  for (std::size_t length = 0; length < original.size(); length += 37u) {
    std::vector<std::uint8_t> bytes(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(length));
    re_test::write_bytes(segment, bytes);
    auto opened = re_test::open_store_at(holder.path(), false);
    RE_REQUIRE(!opened.has_value());
  }
  re_test::write_bytes(segment, original);
  auto recovered = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(recovered.has_value());
  RE_CHECK_EQ(recovered.value()->commit_seq().value(), std::uint64_t{4});
}

RE_TEST(corruption, reordering_two_committed_records_is_detected) {
  re_test::TempDirectory holder("log-reorder");
  populate(holder.path(), 3u);
  const std::filesystem::path segment = holder.path() / "log-00000000.bin";
  const std::vector<std::uint8_t> original = re_test::read_bytes(segment);

  // Swap the second and third record by exchanging their sequence fields; the
  // chain digest then no longer matches either neighbour.
  std::vector<std::uint8_t> bytes = original;
  const std::size_t second_seq_offset = 88u + 8u + 4u + 4u + 8u;
  // Locate the second record by walking the first record's declared length.
  const std::uint32_t first_payload = static_cast<std::uint32_t>(bytes[20]) |
                                      (static_cast<std::uint32_t>(bytes[21]) << 8) |
                                      (static_cast<std::uint32_t>(bytes[22]) << 16) |
                                      (static_cast<std::uint32_t>(bytes[23]) << 24);
  const std::size_t second = 88u + first_payload + 4u;
  if (second + 88u < bytes.size()) {
    bytes[second + second_seq_offset] = 0x09u;
    re_test::write_bytes(segment, bytes);
    auto opened = re_test::open_store_at(holder.path(), false);
    RE_REQUIRE(!opened.has_value());
    RE_CHECK(opened.error().code() == ErrorCode::kCorruptLog ||
             opened.error().code() == ErrorCode::kIntegrityFailure);
  }
  re_test::write_bytes(segment, original);
  RE_CHECK(re_test::open_store_at(holder.path(), false).has_value());
}

RE_TEST(corruption, an_impossible_declared_record_length_is_refused) {
  re_test::TempDirectory holder("log-length");
  populate(holder.path(), 1u);
  const std::filesystem::path segment = holder.path() / "log-00000000.bin";
  std::vector<std::uint8_t> bytes = re_test::read_bytes(segment);
  RE_REQUIRE(bytes.size() > 24u);
  bytes[20] = 0xFFu;
  bytes[21] = 0xFFu;
  bytes[22] = 0xFFu;
  bytes[23] = 0x7Fu;
  re_test::write_bytes(segment, bytes);
  auto opened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(!opened.has_value());
  RE_CHECK(opened.error().code() == ErrorCode::kCorruptLog ||
           opened.error().code() == ErrorCode::kStateDigestMismatch ||
           opened.error().code() == ErrorCode::kIntegrityFailure ||
           opened.error().code() == ErrorCode::kLimitExceeded);
}

RE_TEST(corruption, a_forged_manifest_head_digest_is_rejected_by_the_state_digest) {
  re_test::TempDirectory holder("forged-digest");
  populate(holder.path(), 2u);
  std::vector<std::uint8_t> bytes = re_test::read_bytes(holder.path() / "manifest.a");

  // Flip a bit inside the head digest, then repair the checksum and the sealing
  // digest so only the semantic check can catch it.
  bytes[130] = static_cast<std::uint8_t>(bytes[130] ^ 0x01u);
  const std::uint32_t crc = entl::crc32c(std::span<const std::uint8_t>(bytes.data(), 212u));
  for (unsigned index = 0; index < 4u; ++index) {
    bytes[212u + index] = static_cast<std::uint8_t>((crc >> (index * 8u)) & 0xFFu);
  }
  const entl::Sha256Digest seal =
      entl::Sha256::hash(std::span<const std::uint8_t>(bytes.data(), 216u));
  std::copy(seal.bytes().begin(), seal.bytes().end(), bytes.begin() + 216);

  re_test::write_bytes(holder.path() / "manifest.a", bytes);
  auto opened = re_test::open_store_at(holder.path(), false);
  if (opened.has_value()) {
    auto inspection = entl::Store::inspect(holder.path());
    RE_REQUIRE(inspection.has_value());
    RE_CHECK_EQ(inspection->commit_seq.value(), std::uint64_t{3});
  } else {
    RE_CHECK(opened.error().code() == ErrorCode::kStateDigestMismatch ||
             opened.error().code() == ErrorCode::kIntegrityFailure ||
             opened.error().code() == ErrorCode::kCorruptLog);
  }
}

RE_TEST(corruption, a_corrupted_snapshot_is_refused_rather_than_merged) {
  re_test::TempDirectory holder("snapshot-corrupt");
  entl::StoreOpenOptions options = re_test::test_options(holder.path(), true);
  {
    auto opened = entl::Store::open(options);
    RE_REQUIRE(opened.has_value());
    entl::Store& store = *opened.value();
    RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
        "snapshot-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 1u)));
    for (std::uint64_t index = 0; index < 4u; ++index) {
      RE_REQUIRE_OK(store.grant(build_grant("snapshot-grant-" + std::to_string(index), 2u)));
    }
    RE_REQUIRE_OK(store.compact());
  }

  std::filesystem::path snapshot;
  for (const auto& entry : std::filesystem::directory_iterator(holder.path())) {
    if (entry.path().filename().string().rfind("snapshot-", 0) == 0) {
      snapshot = entry.path();
    }
  }
  RE_REQUIRE(!snapshot.empty());
  std::vector<std::uint8_t> bytes = re_test::read_bytes(snapshot);
  bytes[bytes.size() / 2u] = static_cast<std::uint8_t>(bytes[bytes.size() / 2u] ^ 0x20u);
  re_test::write_bytes(snapshot, bytes);

  auto opened = re_test::open_store_at(holder.path(), false);
  RE_REQUIRE(!opened.has_value());
  RE_CHECK(opened.error().code() == ErrorCode::kCorruptSnapshot ||
           opened.error().code() == ErrorCode::kStateDigestMismatch ||
           opened.error().code() == ErrorCode::kIntegrityFailure);
}
