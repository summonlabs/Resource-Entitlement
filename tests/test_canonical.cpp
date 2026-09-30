// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <vector>

#include "harness.hpp"
#include "resource_entitlement/canonical.hpp"

using entl::CanonicalReader;
using entl::CanonicalWriter;

namespace {

std::vector<std::uint8_t> sample_encoding() {
  CanonicalWriter writer;
  writer.u8(7u);
  writer.u16(0x1234u);
  writer.u32(0xDEADBEEFu);
  writer.u64(0x0102030405060708ull);
  writer.i64(-5);
  writer.boolean(true);
  writer.presence(false);
  writer.presence(true);
  writer.u32(99u);
  writer.fixed(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>("abc"), 3u));
  writer.text("hello");
  writer.text("");
  return writer.data();
}

}  // namespace

RE_TEST(canonical, round_trip_every_primitive) {
  const std::vector<std::uint8_t> bytes = sample_encoding();
  CanonicalReader reader(bytes);
  RE_CHECK_EQ(reader.u8(), std::uint8_t{7u});
  RE_CHECK_EQ(reader.u16(), std::uint16_t{0x1234u});
  RE_CHECK_EQ(reader.u32(), std::uint32_t{0xDEADBEEFu});
  RE_CHECK_EQ(reader.u64(), std::uint64_t{0x0102030405060708ull});
  RE_CHECK_EQ(reader.i64(), std::int64_t{-5});
  RE_CHECK_EQ(reader.boolean(), true);
  RE_CHECK_EQ(reader.presence(), false);
  RE_CHECK_EQ(reader.presence(), true);
  RE_CHECK_EQ(reader.u32(), std::uint32_t{99u});
  const auto fixed = reader.fixed(3u);
  RE_CHECK_EQ(std::string(reinterpret_cast<const char*>(fixed.data()), fixed.size()), std::string("abc"));
  RE_CHECK_EQ(reader.text(16u), std::string("hello"));
  RE_CHECK_EQ(reader.text(16u), std::string(""));
  RE_CHECK(reader.ok());
  RE_CHECK(reader.finish().has_value());
}

RE_TEST(canonical, encoding_is_byte_exact_and_little_endian) {
  const std::vector<std::uint8_t> bytes = sample_encoding();
  RE_REQUIRE(bytes.size() > 6u);
  RE_CHECK_EQ(bytes[0], std::uint8_t{7u});
  RE_CHECK_EQ(bytes[1], std::uint8_t{0x34u});
  RE_CHECK_EQ(bytes[2], std::uint8_t{0x12u});
  RE_CHECK_EQ(bytes[3], std::uint8_t{0xEFu});
  RE_CHECK_EQ(bytes[4], std::uint8_t{0xBEu});
  RE_CHECK_EQ(bytes[5], std::uint8_t{0xADu});
  RE_CHECK_EQ(bytes[6], std::uint8_t{0xDEu});
}

RE_TEST(canonical, truncation_is_detected) {
  const std::vector<std::uint8_t> bytes = sample_encoding();
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    CanonicalReader reader(std::span<const std::uint8_t>(bytes.data(), length));
    (void)reader.u8();
    (void)reader.u16();
    (void)reader.u32();
    (void)reader.u64();
    (void)reader.i64();
    (void)reader.boolean();
    (void)reader.presence();
    (void)reader.presence();
    (void)reader.u32();
    (void)reader.fixed(3u);
    (void)reader.text(16u);
    (void)reader.text(16u);
    auto finished = reader.finish();
    RE_CHECK(!finished.has_value());
  }
}

RE_TEST(canonical, trailing_bytes_are_rejected) {
  std::vector<std::uint8_t> bytes = sample_encoding();
  bytes.push_back(0u);
  CanonicalReader reader(bytes);
  (void)reader.u8();
  (void)reader.u16();
  (void)reader.u32();
  (void)reader.u64();
  (void)reader.i64();
  (void)reader.boolean();
  (void)reader.presence();
  (void)reader.presence();
  (void)reader.u32();
  (void)reader.fixed(3u);
  (void)reader.text(16u);
  (void)reader.text(16u);
  auto finished = reader.finish();
  RE_REQUIRE(!finished.has_value());
  RE_CHECK_EQ(finished.error().code(), entl::ErrorCode::kTrailingBytes);
}

RE_TEST(canonical, hostile_boolean_and_presence_bytes_are_rejected) {
  const std::uint8_t bad_bool[] = {2u};
  CanonicalReader reader(std::span<const std::uint8_t>(bad_bool, 1u));
  (void)reader.boolean();
  RE_CHECK(!reader.ok());

  const std::uint8_t bad_presence[] = {0xFFu};
  CanonicalReader second(std::span<const std::uint8_t>(bad_presence, 1u));
  (void)second.presence();
  RE_CHECK(!second.ok());
}

RE_TEST(canonical, absurd_declared_lengths_are_rejected_without_allocation) {
  CanonicalWriter writer;
  writer.u32(0xFFFFFFFFu);
  CanonicalReader reader(writer.data());
  const auto bytes = reader.bytes(1024u);
  RE_CHECK(bytes.empty());
  RE_CHECK(!reader.ok());
}

RE_TEST(canonical, text_field_validation_is_enforced) {
  CanonicalWriter writer;
  writer.text("bad\x01text");
  CanonicalReader reader(writer.data());
  const std::string text = reader.text(64u);
  RE_CHECK(text.empty());
  RE_CHECK(!reader.ok());

  CanonicalWriter long_writer;
  long_writer.text(std::string(300u, 'x'));
  CanonicalReader long_reader(long_writer.data());
  RE_CHECK(long_reader.text(64u).empty());
  RE_CHECK(!long_reader.ok());
}

RE_TEST(canonical, digest_is_deterministic_for_equal_content) {
  const std::vector<std::uint8_t> first = sample_encoding();
  const std::vector<std::uint8_t> second = sample_encoding();
  RE_CHECK(first == second);
  CanonicalWriter writer_a;
  writer_a.fixed(first);
  CanonicalWriter writer_b;
  writer_b.fixed(second);
  RE_CHECK_EQ(writer_a.digest().to_hex(), writer_b.digest().to_hex());
}
