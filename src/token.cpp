// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "resource_entitlement/token.hpp"

#include <algorithm>
#include <span>
#include <vector>

#include "resource_entitlement/canonical.hpp"
#include "resource_entitlement/digest.hpp"

namespace entl {
namespace {

constexpr std::uint32_t kTokenMagic = 0x4B544552u;  // "RETK"
constexpr std::uint16_t kTokenKind = 1;
constexpr std::size_t kTokenHeaderSize = 4u + 2u + 2u + 4u + 4u + Sha256Digest::kSize;
constexpr std::size_t kMaxTokenBytes = 4u * 1024u * 1024u;

}  // namespace

EntitlementToken EntitlementToken::issue(const Entitlement& record) {
  EntitlementToken token;
  token.record_ = record;
  return token;
}

std::vector<std::uint8_t> EntitlementToken::encode() const {
  CanonicalWriter body;
  record_.encode(body);
  CanonicalWriter writer;
  writer.u32(kTokenMagic);
  writer.u16(kFormatVersion);
  writer.u16(kTokenKind);
  writer.u32(0);
  writer.u32(static_cast<std::uint32_t>(body.size()));
  writer.fixed(Sha256::hash(body.span()).span());
  writer.fixed(body.span());
  return writer.data();
}

Sha256Digest EntitlementToken::token_digest() const {
  const std::vector<std::uint8_t> bytes = encode();
  return Sha256::hash(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

Result<EntitlementToken> EntitlementToken::decode(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kTokenHeaderSize) {
    return Error(ErrorCode::kTruncatedEncoding, "token is shorter than its header");
  }
  if (bytes.size() > kMaxTokenBytes) {
    return Error(ErrorCode::kLimitExceeded, "token exceeds the supported size bound");
  }
  CanonicalReader reader(bytes);
  if (reader.u32() != kTokenMagic) {
    return Error(ErrorCode::kInvalidEncoding, "token magic mismatch");
  }
  const std::uint16_t version = reader.u16();
  const std::uint16_t kind = reader.u16();
  const std::uint32_t reserved = reader.u32();
  const std::uint32_t payload_length = reader.u32();
  const auto digest_bytes = reader.fixed(Sha256Digest::kSize);
  if (!reader.ok()) {
    return Error(ErrorCode::kTruncatedEncoding, "token header could not be decoded");
  }
  const auto body_digest = Sha256Digest::from_bytes(digest_bytes);
  if (!body_digest.has_value()) {
    return Error(ErrorCode::kInvalidDigest, "token body digest field has the wrong length");
  }
  if (version != kFormatVersion || kind != kTokenKind) {
    return Error(ErrorCode::kUnsupportedVersion, "token version or kind is not supported",
                 "version=" + std::to_string(version));
  }
  if (reserved != 0u) {
    return Error(ErrorCode::kReservedFieldNotZero, "token reserved field is not zero");
  }
  if (static_cast<std::size_t>(payload_length) != reader.remaining()) {
    return Error(ErrorCode::kLengthOutOfRange, "token payload length does not match the remaining bytes",
                 "declared=" + std::to_string(payload_length) +
                     " remaining=" + std::to_string(reader.remaining()));
  }
  const std::span<const std::uint8_t> payload = reader.fixed(reader.remaining());
  auto finished = reader.finish();
  if (!finished) {
    return Error(ErrorCode::kInvalidEncoding, "token framing is malformed", finished.error().message());
  }
  if (Sha256::hash(payload) != body_digest.value()) {
    return Error(ErrorCode::kIntegrityFailure, "token body digest does not match its payload");
  }

  CanonicalReader payload_reader(payload);
  EntitlementToken token;
  token.record_ = Entitlement::decode(payload_reader);
  auto payload_finished = payload_reader.finish();
  if (!payload_finished) {
    return Error(ErrorCode::kInvalidEncoding, "token payload is not a well formed entitlement record",
                 payload_finished.error().message());
  }
  CanonicalWriter canonical;
  token.record_.encode(canonical);
  if (canonical.size() != payload.size() ||
      !std::equal(canonical.data().begin(), canonical.data().end(), payload.begin())) {
    return Error(ErrorCode::kInvalidEncoding, "token payload is not in canonical form");
  }
  return token;
}

}  // namespace entl
