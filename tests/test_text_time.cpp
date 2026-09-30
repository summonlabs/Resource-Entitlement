// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <string>

#include "harness.hpp"
#include "resource_entitlement/text.hpp"
#include "resource_entitlement/time.hpp"

using entl::Timestamp;

RE_TEST(text, identifier_grammar) {
  RE_CHECK(entl::is_valid_identifier("a"));
  RE_CHECK(entl::is_valid_identifier("tenant-a"));
  RE_CHECK(entl::is_valid_identifier("tenant_a.b:c-d"));
  RE_CHECK(entl::is_valid_identifier("9lives"));
  RE_CHECK(entl::is_valid_identifier(std::string(64u, 'x')));
  RE_CHECK(!entl::is_valid_identifier(""));
  RE_CHECK(!entl::is_valid_identifier(std::string(65u, 'x')));
  RE_CHECK(!entl::is_valid_identifier("-leading"));
  RE_CHECK(!entl::is_valid_identifier(".leading"));
  RE_CHECK(!entl::is_valid_identifier("trailing."));
  RE_CHECK(!entl::is_valid_identifier("double..dot"));
  RE_CHECK(!entl::is_valid_identifier("has space"));
  RE_CHECK(!entl::is_valid_identifier("has\tslash"));
  RE_CHECK(!entl::is_valid_identifier("has/slash"));
  RE_CHECK(!entl::is_valid_identifier("has\\backslash"));
  RE_CHECK(!entl::is_valid_identifier("nul\0byte"));
  RE_CHECK(!entl::is_valid_identifier("caf\xc3\xa9"));
}

RE_TEST(text, windows_reserved_names_are_rejected) {
  RE_CHECK(!entl::is_valid_identifier("CON"));
  RE_CHECK(!entl::is_valid_identifier("con"));
  RE_CHECK(!entl::is_valid_identifier("Con.txt"));
  RE_CHECK(!entl::is_valid_identifier("NUL"));
  RE_CHECK(!entl::is_valid_identifier("com1"));
  RE_CHECK(!entl::is_valid_identifier("LPT9"));
  RE_CHECK(entl::is_valid_identifier("console"));
  RE_CHECK(entl::is_valid_identifier("com0"));
  RE_CHECK(entl::is_valid_identifier("com10"));
  RE_CHECK(entl::is_windows_reserved_name("aux.bin"));
  RE_CHECK(!entl::is_windows_reserved_name("auxiliary"));
}

RE_TEST(text, utf8_validation_rejects_hostile_sequences) {
  RE_CHECK(entl::is_valid_utf8("plain ascii"));
  RE_CHECK(entl::is_valid_utf8("\xc3\xa9"));
  RE_CHECK(entl::is_valid_utf8("\xe2\x82\xac"));
  RE_CHECK(entl::is_valid_utf8("\xf0\x9f\x9a\x80"));
  RE_CHECK(!entl::is_valid_utf8("\xc0\x80"));
  RE_CHECK(!entl::is_valid_utf8("\xc1\xbf"));
  RE_CHECK(!entl::is_valid_utf8("\xe0\x80\xaf"));
  RE_CHECK(!entl::is_valid_utf8("\xed\xa0\x80"));
  RE_CHECK(!entl::is_valid_utf8("\xed\xbf\xbf"));
  RE_CHECK(!entl::is_valid_utf8("\xf4\x90\x80\x80"));
  RE_CHECK(!entl::is_valid_utf8("\xf5\x80\x80\x80"));
  RE_CHECK(!entl::is_valid_utf8("\xff"));
  RE_CHECK(!entl::is_valid_utf8("\xfe"));
  RE_CHECK(!entl::is_valid_utf8("\x80"));
  RE_CHECK(!entl::is_valid_utf8("\xc3"));
  RE_CHECK(!entl::is_valid_utf8("truncated\xe2\x82"));
}

RE_TEST(text, text_rules_reject_controls_and_bound_length) {
  RE_CHECK(entl::is_valid_text(""));
  RE_CHECK(entl::is_valid_text("a note with spaces and \xc3\xa9 accents"));
  RE_CHECK(!entl::is_valid_text("bell\x07"));
  RE_CHECK(!entl::is_valid_text("newline\n"));
  RE_CHECK(!entl::is_valid_text("escape\x1b"));
  RE_CHECK(!entl::is_valid_text("del\x7f"));
  RE_CHECK(!entl::is_valid_text("c1\xc2\x9b"));
  RE_CHECK(entl::is_valid_text(std::string(256u, 'x')));
  RE_CHECK(!entl::is_valid_text(std::string(257u, 'x')));
}

RE_TEST(text, case_and_truncation_helpers) {
  RE_CHECK_EQ(entl::to_lower_ascii("AbC-1"), std::string("abc-1"));
  RE_CHECK(entl::equals_ascii_ignore_case("Tenant-A", "tenant-a"));
  RE_CHECK(!entl::equals_ascii_ignore_case("a", "ab"));
  RE_CHECK_EQ(entl::truncate_utf8("\xc3\xa9\xc3\xa9", 3u), std::string("\xc3\xa9"));
  RE_CHECK_EQ(entl::truncate_utf8("abc", 2u), std::string("ab"));
  RE_CHECK_EQ(entl::truncate_utf8("abc", 9u), std::string("abc"));
}

RE_TEST(time, rfc3339_round_trip) {
  const std::string samples[] = {
      "1970-01-01T00:00:00.000000000Z",
      "2024-02-29T23:59:59.999999999Z",
      "2000-02-29T12:34:56.000000001Z",
      "1999-12-31T23:59:59.000000000Z",
      "2262-04-11T23:47:16.854775807Z",
  };
  for (const std::string& sample : samples) {
    auto parsed = Timestamp::parse(sample);
    RE_REQUIRE(parsed.has_value());
    RE_CHECK_EQ(parsed->to_rfc3339(), sample);
  }
}

RE_TEST(time, parse_rejects_malformed_and_out_of_range) {
  const std::string bad[] = {
      "", "not-a-time", "2024-13-01T00:00:00Z", "2024-00-01T00:00:00Z", "2024-01-00T00:00:00Z",
      "2024-01-32T00:00:00Z", "2023-02-29T00:00:00Z", "2024-01-01T24:00:00Z", "2024-01-01T00:60:00Z",
      "2024-01-01T00:00:60Z", "2024-01-01T00:00:00", "2024-01-01T00:00:00+01:00",
      "2024-01-01T00:00:00.Z", "2024-01-01T00:00:00.1234567890Z", "1969-12-31T23:59:59Z",
      "2024-1-01T00:00:00Z", "2024-01-01 00:00:00",
  };
  for (const std::string& sample : bad) {
    RE_CHECK(!Timestamp::parse(sample).has_value());
  }
  RE_CHECK(!Timestamp::from_unix_nanos(-1).has_value());
  RE_CHECK(Timestamp::from_unix_nanos(0).has_value());
}

RE_TEST(time, parse_accepts_integer_nanoseconds_and_lowercase_separators) {
  auto parsed = Timestamp::parse("1700000000000000000");
  RE_REQUIRE(parsed.has_value());
  RE_CHECK_EQ(parsed->unix_nanos(), std::int64_t{1700000000000000000});
  auto lowercase = Timestamp::parse("2024-01-01t00:00:00z");
  RE_REQUIRE(lowercase.has_value());
  RE_CHECK_EQ(lowercase->to_rfc3339(), std::string("2024-01-01T00:00:00.000000000Z"));
  auto short_fraction = Timestamp::parse("2024-01-01T00:00:00.5Z");
  RE_REQUIRE(short_fraction.has_value());
  RE_CHECK_EQ(short_fraction->unix_nanos(), std::int64_t{1704067200500000000});
  auto too_long_integer = Timestamp::parse("17000000000000000000");
  RE_CHECK(!too_long_integer.has_value());
}

RE_TEST(time, arithmetic_is_checked) {
  auto base = Timestamp::from_unix_nanos(1000);
  RE_REQUIRE(base.has_value());
  auto added = base->checked_add_nanos(500);
  RE_REQUIRE(added.has_value());
  RE_CHECK_EQ(added->unix_nanos(), std::int64_t{1500});
  RE_CHECK(!base->checked_add_nanos(-1001).has_value());
  auto huge = Timestamp::from_unix_nanos(std::numeric_limits<std::int64_t>::max());
  RE_REQUIRE(huge.has_value());
  RE_CHECK(!huge->checked_add_nanos(1).has_value());
  RE_CHECK_EQ(base->checked_distance_nanos(*added).value(), std::int64_t{500});
  RE_CHECK_EQ(base->checked_distance_nanos(*base).value(), std::int64_t{0});
}

RE_TEST(time, manual_clock_is_deterministic) {
  entl::ManualClock clock(entl::Timestamp::from_unix_nanos(42).value());
  RE_CHECK_EQ(clock.now().unix_nanos(), std::int64_t{42});
  clock.set(entl::Timestamp::from_unix_nanos(7).value());
  RE_CHECK_EQ(clock.now().unix_nanos(), std::int64_t{7});
  clock.advance_nanos(3);
  RE_CHECK_EQ(clock.now().unix_nanos(), std::int64_t{10});
}

RE_TEST(time, duration_formatting) {
  RE_CHECK_EQ(entl::format_nanos(500), std::string("500 ns"));
  RE_CHECK_EQ(entl::format_nanos(2500), std::string("2.5 us"));
  RE_CHECK_EQ(entl::format_nanos(12345678), std::string("12.3 ms"));
  RE_CHECK_EQ(entl::format_nanos(2500000000LL), std::string("2.5 s"));
}
