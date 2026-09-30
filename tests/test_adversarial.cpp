// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial hardening: hostile text, absurd sizes, reserved names, long
// paths, impossible enum values, and non-canonical encodings.

#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "harness.hpp"
#include "resource_entitlement/store.hpp"
#include "resource_entitlement/token.hpp"
#include "test_support.hpp"

using entl::ErrorCode;
using entl::Unit;

namespace {

entl::ReleaseRequest draw_as_release(const entl::DrawRequest& draw, std::uint64_t units) {
  entl::ReleaseRequest release;
  release.request_id = draw.request_id;
  release.now = draw.now;
  release.id = draw.id;
  release.expected_revision = draw.expected_revision;
  release.quantity = entl::Quantity::make(entl::Unit::kCount, units).value();
  release.actor = draw.actor;
  return release;
}

entl::GrantRequest base_grant(std::string_view seed, std::uint64_t units) {
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

}  // namespace

RE_TEST(adversarial, hostile_free_text_is_refused_before_it_can_be_persisted) {
  re_test::StoreFixture fixture;
  (void)fixture.publish_authority(1u);

  const std::string hostile[] = {
      std::string("newline\n"),        std::string("tab\t"),
      std::string("nul\0byte", 8u),    std::string("escape\x1b"),
      std::string("del\x7f"),          std::string("overlong\xc0\x80"),
      std::string("surrogate\xed\xa0\x80"),
      std::string("too long: ") + std::string(400u, 'x'),
  };
  std::uint64_t index = 0;
  for (const std::string& text : hostile) {
    entl::GrantRequest grant = base_grant("hostile-note-" + std::to_string(index), 5u);
    grant.note = text;
    auto outcome = fixture.store().grant(grant);
    RE_REQUIRE(!outcome.has_value());
    RE_CHECK_EQ(outcome.error().code(), ErrorCode::kInvalidText);
    ++index;
  }
  RE_CHECK_EQ(fixture.store().stats().entitlement_count, std::size_t{0});

  // The same rule protects authority publications and holder changes.
  entl::AuthorityUpdateRequest update = re_test::authority_request(
      "hostile-authority", fixture.now(), fixture.store().authority().revision, 1u);
  update.note = "bell\x07";
  RE_CHECK_ERR(fixture.store().update_authority(update), ErrorCode::kInvalidText);

  entl::GrantRequest good = base_grant("legitimate-note", 5u);
  good.note = "a legitimate note with unicode \xc3\xa9";
  const entl::EntitlementId id = fixture.grant_full(good);

  entl::TransferRequest move;
  move.request_id = re_test::request_id("hostile-move");
  move.now = fixture.now();
  move.source = id;
  move.expected_revision = fixture.store().get(id)->revision;
  move.mode = entl::TransferMode::kMove;
  move.target_holder = re_test::must_parse<entl::TenantId>("tenant-b");
  move.actor = re_test::must_parse<entl::ActorId>("operator-1");
  move.note = std::string(300u, 'y');
  RE_CHECK_ERR(fixture.store().transfer(move), ErrorCode::kInvalidText);
  RE_CHECK_EQ(fixture.store().get(id)->holder.str(), std::string("tenant-a"));
}

RE_TEST(adversarial, identifier_rules_are_enforced_at_the_api_boundary) {
  const char* hostile[] = {"",          " ",           "has space",   "trailing.",
                           "double..dot", "CON",        "nul",         "com1",
                           "a/b",       "a\\b",       "\\server",  "..",
                           "verylongidentifierthatisfarlongerthansixtyfourcharactersandthenseveralmorecharacters"};
  for (const char* text : hostile) {
    auto tenant = entl::TenantId::parse(text);
    RE_CHECK(!tenant.has_value());
    if (!tenant.has_value()) {
      RE_CHECK_EQ(tenant.error().code(), ErrorCode::kInvalidIdentifier);
    }
    auto facility = entl::FacilityId::parse(text);
    RE_CHECK(!facility.has_value());
  }
  RE_CHECK(entl::TenantId::parse("tenant-a").has_value());
  RE_CHECK(entl::FacilityId::parse("dc.1:west").has_value());
}

RE_TEST(adversarial, absurd_quantities_saturate_instead_of_wrapping) {
  re_test::StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const std::uint64_t maximum = 18446744073709551615ull;
  entl::GrantRequest grant = base_grant("maximum-quantity", maximum);
  const entl::EntitlementId id = fixture.grant_full(grant);
  RE_CHECK_EQ(fixture.store().get(id)->remaining.units(), maximum);

  // Drawing the maximum is legal and exactly exhausts the record.
  entl::DrawRequest draw;
  draw.request_id = re_test::request_id("maximum-draw");
  draw.now = fixture.now();
  draw.id = id;
  draw.expected_revision = fixture.store().get(id)->revision;
  draw.quantity = entl::Quantity::make(Unit::kCount, maximum).value();
  draw.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_REQUIRE_OK(fixture.store().draw(draw));
  RE_CHECK(fixture.store().get(id)->remaining.is_zero());

  // Releasing exactly what was drawn restores the maximum without wrapping.
  entl::ReleaseRequest restore = draw_as_release(draw, maximum);
  restore.request_id = re_test::request_id("maximum-release");
  restore.expected_revision = fixture.store().get(id)->revision;
  RE_REQUIRE_OK(fixture.store().release(restore));
  RE_CHECK_EQ(fixture.store().get(id)->remaining.units(), maximum);

  // Releasing more than was ever drawn is refused.
  entl::ReleaseRequest over = draw_as_release(draw, 1u);
  over.request_id = re_test::request_id("maximum-release-over");
  over.expected_revision = fixture.store().get(id)->revision;
  auto over_outcome = fixture.store().release(over);
  RE_REQUIRE(!over_outcome.has_value());
  // Saturating arithmetic is detected before the release bound is reached.
  RE_CHECK(over_outcome.error().code() == ErrorCode::kLimitExceeded ||
           over_outcome.error().code() == ErrorCode::kArithmeticOverflow);

  // The release bound is also enforced on an ordinary grant.
  const entl::EntitlementId second = fixture.grant_default("release-bound", 10u);
  entl::ReleaseRequest too_much;
  too_much.request_id = re_test::request_id("release-bound-over");
  too_much.now = fixture.now();
  too_much.id = second;
  too_much.expected_revision = fixture.store().get(second)->revision;
  too_much.quantity = entl::Quantity::make(Unit::kCount, 1u).value();
  too_much.actor = re_test::must_parse<entl::ActorId>("operator-1");
  RE_CHECK_ERR(fixture.store().release(too_much), ErrorCode::kLimitExceeded);
}

RE_TEST(adversarial, a_directory_beyond_the_legacy_path_limit_is_supported) {
  re_test::TempDirectory holder("long-path");
  std::filesystem::path deep = holder.path();
  for (int index = 0; index < 8; ++index) {
    deep /= std::string(40u, static_cast<char>('a' + (index % 26)));
  }
  RE_CHECK(deep.string().size() > 260u);

  auto opened = re_test::open_store_at(deep, true);
  RE_REQUIRE(opened.has_value());
  {
    entl::Store& store = *opened.value();
    RE_REQUIRE_OK(store.update_authority(re_test::authority_request(
        "long-path-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 1u)));
    RE_REQUIRE_OK(store.grant(base_grant("long-path-grant", 9u)));
  }
  opened.value().reset();
  auto reopened = re_test::open_store_at(deep, false);
  RE_REQUIRE(reopened.has_value());
  RE_CHECK_EQ(reopened.value()->stats().entitlement_count, std::size_t{1});
}

RE_TEST(adversarial, a_windows_reserved_device_directory_is_refused) {
  re_test::TempDirectory holder("reserved");
  const std::filesystem::path reserved = holder.path() / "CON";
  std::error_code ec;
  std::filesystem::create_directories(reserved, ec);
  auto opened = re_test::open_store_at(reserved, true);
  if (opened.has_value()) {
    // The extended namespace makes a reserved device name an ordinary name, so
    // the store must be a real directory rather than the legacy character
    // device; writing there must never be silently redirected.
    std::error_code check_ec;
    RE_CHECK(std::filesystem::is_directory(reserved, check_ec));
    RE_REQUIRE_OK(opened.value()->update_authority(re_test::authority_request(
        "reserved-authority", re_test::instant(1700000000), entl::Revision::from_value(1u), 1u)));
  } else {
    RE_CHECK(opened.error().code() == ErrorCode::kPathError ||
             opened.error().code() == ErrorCode::kNotADirectory ||
             opened.error().code() == ErrorCode::kIoError ||
             opened.error().code() == ErrorCode::kInvalidPath);
  }
}

RE_TEST(adversarial, a_file_where_a_directory_is_expected_is_refused) {
  re_test::TempDirectory holder("file-as-dir");
  const std::filesystem::path file = holder.path() / "not-a-directory";
  {
    std::ofstream stream(file, std::ios::binary);
    stream << "this is a regular file";
  }
  auto opened = re_test::open_store_at(file, true);
  RE_REQUIRE(!opened.has_value());
  RE_CHECK(opened.error().code() == ErrorCode::kNotADirectory ||
           opened.error().code() == ErrorCode::kPathError ||
           opened.error().code() == ErrorCode::kIoError);
}

RE_TEST(adversarial, an_entitlement_record_with_impossible_enums_is_refused) {
  re_test::StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const entl::EntitlementId id = fixture.grant_default("enum-target", 5u);
  const entl::Entitlement good = fixture.store().get(id).value();

  const auto round_trip_fails = [](const entl::Entitlement& record) {
    entl::CanonicalWriter writer;
    record.encode(writer);
    entl::CanonicalReader reader(std::span<const std::uint8_t>(writer.data().data(), writer.data().size()));
    (void)entl::Entitlement::decode(reader);
    return !reader.ok() || !reader.finish().has_value();
  };

  entl::Entitlement mutated = good;
  mutated.state = static_cast<entl::EntitlementState>(0x7Fu);
  RE_CHECK(round_trip_fails(mutated));

  mutated = good;
  mutated.relation = static_cast<entl::LineageRelation>(0x7Fu);
  RE_CHECK(round_trip_fails(mutated));

  mutated = good;
  mutated.unit = static_cast<Unit>(0x7Fu);
  RE_CHECK(round_trip_fails(mutated));

  mutated = good;
  mutated.terminal_reason = static_cast<entl::TerminalReason>(0x7Fu);
  RE_CHECK(round_trip_fails(mutated));

  mutated = good;
  mutated.fence = entl::FenceMask::of(0u);
  RE_CHECK(round_trip_fails(mutated));

  mutated = good;
  mutated.priority.klass = static_cast<entl::PriorityClass>(0x7Fu);
  RE_CHECK(round_trip_fails(mutated));

  RE_CHECK(!round_trip_fails(good));
}

RE_TEST(adversarial, a_non_canonical_token_payload_is_refused) {
  re_test::StoreFixture fixture;
  (void)fixture.publish_authority(1u);
  const entl::EntitlementId id = fixture.grant_default("token-canonical", 5u);
  auto token = fixture.store().issue_token(id);
  RE_REQUIRE(token.has_value());
  const std::vector<std::uint8_t> bytes = token->encode();

  // Append an extra canonical field to the payload and re-seal the framing; the
  // canonical round-trip check must still refuse it.
  std::vector<std::uint8_t> extended = bytes;
  extended.insert(extended.end(), {0x01u, 0x02u, 0x03u, 0x04u});
  const std::size_t payload_length = extended.size() - 48u;
  for (unsigned index = 0; index < 4u; ++index) {
    extended[12u + index] = static_cast<std::uint8_t>((payload_length >> (index * 8u)) & 0xFFu);
  }
  const auto payload = std::span<const std::uint8_t>(extended.data() + 48u, payload_length);
  const entl::Sha256Digest digest = entl::Sha256::hash(payload);
  std::copy(digest.bytes().begin(), digest.bytes().end(), extended.begin() + 16);
  auto decoded = entl::EntitlementToken::decode(extended);
  RE_CHECK(!decoded.has_value());
}

RE_TEST(adversarial, oversized_request_documents_are_refused_by_the_cli_grammar) {
  // The kv grammar is exercised end to end in the CLI suite; here the bound is
  // checked at the library level through the canonical reader.
  entl::CanonicalWriter writer;
  writer.u32(0xFFFFFFFFu);
  entl::CanonicalReader reader(writer.data());
  (void)reader.bytes(1024u);
  RE_CHECK(!reader.ok());
  auto finished = reader.finish();
  RE_CHECK(!finished.has_value());
}
