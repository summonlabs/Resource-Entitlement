// Resource Entitlement — version and build identification.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_VERSION_HPP
#define RESOURCE_ENTITLEMENT_VERSION_HPP

#include <cstdint>

namespace entl {

inline constexpr std::uint16_t kVersionMajor = 1;
inline constexpr std::uint16_t kVersionMinor = 0;
inline constexpr std::uint16_t kVersionPatch = 0;

/// Semantic version of the library, e.g. "1.0.0".
[[nodiscard]] const char* version_string() noexcept;

/// "Debug" or "Release" depending on NDEBUG at compile time of the library.
[[nodiscard]] const char* build_mode_string() noexcept;

/// Durable store format version written into every manifest and snapshot.
inline constexpr std::uint16_t kStoreFormatVersion = 1;

/// File magic values. These are the first bytes of the corresponding object.
inline constexpr std::uint32_t kManifestMagic = 0x464D4552u;  // "REMF"
inline constexpr std::uint32_t kLogRecordMagic = 0x43524552u; // "RERC"
inline constexpr std::uint32_t kSnapshotMagic = 0x504E5352u;  // "RSNP"
inline constexpr std::uint32_t kFenceMagic = 0x4E454652u;     // "RFEN"

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_VERSION_HPP
