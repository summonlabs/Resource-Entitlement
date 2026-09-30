// Resource Entitlement — canonical entitlement tokens.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_TOKEN_HPP
#define RESOURCE_ENTITLEMENT_TOKEN_HPP

#include <cstdint>
#include <span>
#include <vector>

#include "resource_entitlement/entitlement.hpp"
#include "resource_entitlement/error.hpp"

namespace entl {

/// A canonical, self-describing transport encoding of one entitlement record.
///
/// A token is a record encoding, not a bearer credential: it is not signed and
/// it carries no secret, so possessing a token confers no authority. Authority
/// is decided only by the durable ledger, and every verification re-reads that
/// ledger. The token exists so that a caller can hand an exact, digestible
/// snapshot of a record to another process without a shared schema.
class EntitlementToken {
public:
  static constexpr std::uint16_t kFormatVersion = 1;

  EntitlementToken() = default;

  [[nodiscard]] static EntitlementToken issue(const Entitlement& record);

  /// Strict decode. Fails if the magic, version, kind, reserved fields, length,
  /// body digest, or canonical round trip do not match exactly.
  [[nodiscard]] static Result<EntitlementToken> decode(std::span<const std::uint8_t> bytes);

  [[nodiscard]] std::vector<std::uint8_t> encode() const;
  [[nodiscard]] Sha256Digest token_digest() const;

  [[nodiscard]] const Entitlement& record() const noexcept { return record_; }
  [[nodiscard]] const EntitlementId& id() const noexcept { return record_.id; }

private:
  Entitlement record_{};
};

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_TOKEN_HPP
