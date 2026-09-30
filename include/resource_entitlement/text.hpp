// Resource Entitlement — bounded ASCII identifiers and strict UTF-8 text.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_TEXT_HPP
#define RESOURCE_ENTITLEMENT_TEXT_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace entl {

/// Maximum length of an identifier in bytes.
inline constexpr std::size_t kMaxIdentifierBytes = 64;

/// Maximum length of free text (note/reason/detail) in bytes.
inline constexpr std::size_t kMaxTextBytes = 256;

/// Maximum length of a display name or human message carried by the library.
inline constexpr std::size_t kMaxMessageBytes = 512;

/// Identifier grammar (checked by is_valid_identifier and Identifier::parse):
///   * 1 .. 64 bytes, ASCII only;
///   * first byte in [A-Za-z0-9_];
///   * remaining bytes in [A-Za-z0-9._:-];
///   * must not end with '.';
///   * must not contain "..";
///   * must not be a Windows reserved device name (CON, PRN, AUX, NUL,
///     COM1..COM9, LPT1..LPT9), compared case-insensitively before the first
///     '.'.
/// Identifiers are never interpreted as path components by this library; the
/// trailing-dot, dot-dot, and reserved-name rules are defence in depth for
/// callers that do use them as file or directory names.
[[nodiscard]] bool is_valid_identifier(std::string_view text) noexcept;

/// Strict UTF-8 validation: rejects overlong encodings, surrogate code points,
/// code points above U+10FFFF, truncated sequences, and 0xC0/0xC1/0xF5..0xFF
/// lead bytes.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

/// A bounded, control-character-free, strictly valid UTF-8 string suitable for
/// notes, reasons, and diagnostics. Empty text is allowed (it means "not
/// supplied", which is modelled explicitly by the caller).
[[nodiscard]] bool is_valid_text(std::string_view text) noexcept;

/// ASCII-only lowercase conversion. Bytes outside [A-Z] are copied unchanged.
[[nodiscard]] std::string to_lower_ascii(std::string_view text);

/// Case-insensitive ASCII equality.
[[nodiscard]] bool equals_ascii_ignore_case(std::string_view a, std::string_view b) noexcept;

/// True when the byte sequence terminates a Windows reserved device name.
[[nodiscard]] bool is_windows_reserved_name(std::string_view text) noexcept;

/// Truncates to at most p limit bytes without splitting a UTF-8 sequence.
[[nodiscard]] std::string_view truncate_utf8(std::string_view text, std::size_t limit) noexcept;

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_TEXT_HPP
