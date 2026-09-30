// Resource Entitlement — error codes, error objects, and Result.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_ERROR_HPP
#define RESOURCE_ENTITLEMENT_ERROR_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>

namespace entl {

/// Stable, enumerated refusal/outcome codes.
///
/// The numeric values are part of the durable format (they are persisted inside
/// idempotency records) and must never be renumbered. New codes are appended at
/// the end of their group.
enum class ErrorCode : std::uint16_t {
  kOk = 0,

  // --- input validation (1..19) -------------------------------------------
  kInvalidArgument = 1,
  kInvalidIdentifier = 2,
  kInvalidText = 3,
  kInvalidDigest = 4,
  kInvalidTimestamp = 5,
  kInvalidEnumValue = 6,
  kInvalidQuantity = 7,
  kInvalidUnit = 8,
  kInvalidPriority = 9,
  kTooLong = 10,
  kMalformedInput = 11,
  kUnexpectedField = 12,
  kMissingField = 13,
  kDuplicateField = 14,
  kUnsupportedVersion = 15,
  kInvalidPath = 16,
  kInvalidFenceMask = 17,
  kReservedFieldNotZero = 18,

  // --- canonical codec (20..29) ------------------------------------------
  kTruncatedEncoding = 20,
  kTrailingBytes = 21,
  kInvalidEncoding = 22,
  kLengthOutOfRange = 23,

  // --- domain / lifecycle (30..69) ---------------------------------------
  kUnknownEntitlement = 30,
  kRevisionConflict = 31,
  kRequestIdConflict = 32,
  kInvalidTransition = 33,
  kInsufficientQuantity = 34,
  kQuantityExceeded = 35,
  kUnitMismatch = 36,
  kScopeMismatch = 37,
  kTenantMismatch = 38,
  kServiceMismatch = 39,
  kServiceClassMismatch = 40,
  kStaleBinding = 41,
  kStaleEpoch = 42,
  kNotYetEffective = 43,
  kExpired = 44,
  kRevoked = 45,
  kSuspended = 46,
  kSuperseded = 47,
  kArithmeticOverflow = 48,
  kArithmeticUnderflow = 49,
  kLimitExceeded = 50,
  kMissingAdmissionEvidence = 51,
  kLineageViolation = 52,
  kConflict = 53,
  kSelfReference = 54,
  kNotAuthorizedHolder = 55,
  kWindowViolation = 56,
  kCapacityExhausted = 57,
  kDuplicateIdentity = 58,
  kNoRecordedOutcome = 59,

  // --- durability / storage (70..99) -------------------------------------
  kIoError = 70,
  kPathError = 71,
  kWriterLockHeld = 72,
  kStoreNotFound = 73,
  kStoreAlreadyExists = 74,
  kCorruptManifest = 75,
  kCorruptLog = 76,
  kCorruptSnapshot = 77,
  kIntegrityFailure = 78,
  kStateDigestMismatch = 79,
  kRollbackDetected = 80,
  kReadOnlyStore = 81,
  kSegmentFull = 82,
  kNotADirectory = 83,
  kUnsupportedFormat = 84,
  kSnapshotInconsistent = 85,
  kLockError = 86,

  // --- internal (100..109) -----------------------------------------------
  kInternalError = 100,
};

/// Canonical short name, e.g. "stale_binding". Stable across releases.
[[nodiscard]] const char* error_code_name(ErrorCode code) noexcept;

/// True when the code reports authority invalidated by fencing, staleness, or
/// terminal lifecycle state rather than bad input.
[[nodiscard]] bool error_code_is_authority_refusal(ErrorCode code) noexcept;

/// True for 1..29 (caller-supplied input or encoding was rejected).
[[nodiscard]] bool error_code_is_validation(ErrorCode code) noexcept;

/// True for 70..99 (durable storage or operating-system failure).
[[nodiscard]] bool error_code_is_durability(ErrorCode code) noexcept;

/// A refusal. Always carries a stable code; the message is human-readable
/// explanatory text and is never parsed by the library.
class Error {
public:
  Error() = default;
  Error(ErrorCode code, std::string message, std::string detail = {})
      : code_(code), message_(std::move(message)), detail_(std::move(detail)) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
  [[nodiscard]] const char* code_name() const noexcept { return error_code_name(code_); }

  /// Single-line rendering: "<code>: <message>".
  [[nodiscard]] std::string to_string() const;

private:
  ErrorCode code_{ErrorCode::kInternalError};
  std::string message_;
  std::string detail_;
};

/// Either a value or an Error. Never holds both, never holds neither.
template <class T>
class [[nodiscard]] Result {
public:
  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] T* operator->() { return std::addressof(std::get<0>(storage_)); }
  [[nodiscard]] const T* operator->() const { return std::addressof(std::get<0>(storage_)); }
  [[nodiscard]] T& operator*() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& operator*() const& { return std::get<0>(storage_); }

  [[nodiscard]] Error& error() & { return std::get<1>(storage_); }
  [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }

  T value_or(T fallback) const { return has_value() ? std::get<0>(storage_) : std::move(fallback); }

private:
  std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
  Result() = default;
  Result(Error error) : error_(std::move(error)), ok_(false) {}

  [[nodiscard]] bool has_value() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }
  [[nodiscard]] Error& error() & { return error_; }
  [[nodiscard]] const Error& error() const& { return error_; }

private:
  Error error_{};
  bool ok_{true};
};

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_ERROR_HPP
