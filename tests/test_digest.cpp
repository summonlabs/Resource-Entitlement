// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <array>
#include <string>
#include <vector>

#include "harness.hpp"
#include "resource_entitlement/digest.hpp"

using entl::Sha256;
using entl::Sha256Digest;

namespace {

std::string hex_of(std::string_view text) { return Sha256::hash(text).to_hex(); }

std::string pad_left(std::string value, std::size_t width) {
  while (value.size() < width) {
    value.insert(value.begin(), '0');
  }
  return value;
}

}  // namespace

RE_TEST(digest, sha256_known_answer_vectors) {
  RE_CHECK_EQ(hex_of(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  RE_CHECK_EQ(hex_of("abc"),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  RE_CHECK_EQ(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  RE_CHECK_EQ(hex_of("The quick brown fox jumps over the lazy dog"),
              std::string("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));
}

RE_TEST(digest, sha256_million_a) {
  Sha256 hasher;
  std::string block(1000u, 'a');
  for (int i = 0; i < 1000; ++i) {
    hasher.update(block);
  }
  RE_CHECK_EQ(hasher.finish().to_hex(),
              std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

RE_TEST(digest, sha256_length_boundaries_match_single_shot) {
  // Lengths around every block and padding boundary must agree between the
  // streaming and one-shot paths.
  for (std::size_t length = 0; length <= 200; ++length) {
    std::string data(length, static_cast<char>('a' + (length % 26)));
    Sha256 streamed;
    std::size_t offset = 0;
    std::size_t step = 1;
    while (offset < data.size()) {
      const std::size_t take = (step > data.size() - offset) ? data.size() - offset : step;
      streamed.update(std::string_view(data).substr(offset, take));
      offset += take;
      step = (step * 3u) % 17u + 1u;
    }
    RE_CHECK_EQ(streamed.finish().to_hex(), Sha256::hash(data).to_hex());
    RE_CHECK_EQ(hex_of(data), Sha256::hash(data).to_hex());
  }
}

RE_TEST(digest, sha256_finish_is_idempotent_and_update_after_finish_is_ignored) {
  Sha256 hasher;
  hasher.update("abc");
  const Sha256Digest first = hasher.finish();
  hasher.update("def");
  const Sha256Digest second = hasher.finish();
  RE_CHECK_EQ(first.to_hex(), second.to_hex());
}

RE_TEST(digest, sha256_digest_hex_round_trip_and_rejection) {
  const Sha256Digest digest = Sha256::hash("round-trip");
  auto parsed = Sha256Digest::from_hex(digest.to_hex());
  RE_REQUIRE(parsed.has_value());
  RE_CHECK_EQ(parsed->to_hex(), digest.to_hex());

  RE_CHECK(!Sha256Digest::from_hex("").has_value());
  RE_CHECK(!Sha256Digest::from_hex("ab").has_value());
  RE_CHECK(!Sha256Digest::from_hex(std::string(63u, 'a')).has_value());
  RE_CHECK(!Sha256Digest::from_hex(std::string(65u, 'a')).has_value());
  RE_CHECK(!Sha256Digest::from_hex(std::string(64u, 'z')).has_value());
  RE_CHECK(Sha256Digest::from_hex(std::string(64u, 'A')).has_value());
  RE_CHECK(Sha256Digest{}.is_zero());
  RE_CHECK(!digest.is_zero());
}

RE_TEST(digest, crc32c_known_answer_vectors) {
  const auto crc = [](std::string_view text) {
    return entl::crc32c(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                                      text.size()));
  };
  RE_CHECK_EQ(pad_left(std::to_string(crc("")), 8u), std::string("00000000"));
  RE_CHECK_EQ(pad_left(std::to_string(crc("123456789")), 8u), std::to_string(0xE3069283u));
  RE_CHECK_EQ(pad_left(std::to_string(crc("The quick brown fox jumps over the lazy dog")), 8u),
              std::to_string(0x22620404u));
}

RE_TEST(digest, crc32c_extend_matches_single_shot) {
  const auto bytes = [](std::string_view text) {
    return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  };
  const std::string left = "resource";
  const std::string right = "-entitlement";
  const std::uint32_t combined = entl::crc32c_extend(entl::crc32c(bytes(left)), bytes(right));
  RE_CHECK_EQ(combined, entl::crc32c(bytes(left + right)));
}

RE_TEST(digest, hex_helpers_round_trip) {
  const std::array<std::uint8_t, 5> raw = {0x00u, 0x01u, 0x7Fu, 0x80u, 0xFFu};
  const std::string text = entl::bytes_to_hex(std::span<const std::uint8_t>(raw.data(), raw.size()));
  RE_CHECK_EQ(text, std::string("00017f80ff"));
  auto decoded = entl::hex_to_bytes(text, 5u);
  RE_REQUIRE(decoded.has_value());
  RE_CHECK_EQ(decoded->size(), std::size_t{5});
  RE_CHECK_EQ((*decoded)[4], std::uint8_t{0xFFu});
  RE_CHECK(!entl::hex_to_bytes("abc", 0u).has_value());
  RE_CHECK(!entl::hex_to_bytes("zz", 0u).has_value());
  RE_CHECK(!entl::hex_to_bytes("aabb", 3u).has_value());
}
