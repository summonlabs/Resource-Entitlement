// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <array>
#include <cstring>

#include "resource_entitlement/digest.hpp"
#include "resource_entitlement/entitlement.hpp"
#include "resource_entitlement/events.hpp"
#include "resource_entitlement/priority.hpp"
#include "resource_entitlement/quantity.hpp"
#include "resource_entitlement/scope.hpp"
#include "resource_entitlement/store.hpp"
#include "resource_entitlement/strong.hpp"
#include "resource_entitlement/text.hpp"
#include "resource_entitlement/token.hpp"

namespace entl {

// ---------------------------------------------------------------------------
// ErrorCode
// ---------------------------------------------------------------------------

const char* error_code_name(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kOk: return "ok";
    case ErrorCode::kInvalidArgument: return "invalid_argument";
    case ErrorCode::kInvalidIdentifier: return "invalid_identifier";
    case ErrorCode::kInvalidText: return "invalid_text";
    case ErrorCode::kInvalidDigest: return "invalid_digest";
    case ErrorCode::kInvalidTimestamp: return "invalid_timestamp";
    case ErrorCode::kInvalidEnumValue: return "invalid_enum_value";
    case ErrorCode::kInvalidQuantity: return "invalid_quantity";
    case ErrorCode::kInvalidUnit: return "invalid_unit";
    case ErrorCode::kInvalidPriority: return "invalid_priority";
    case ErrorCode::kTooLong: return "too_long";
    case ErrorCode::kMalformedInput: return "malformed_input";
    case ErrorCode::kUnexpectedField: return "unexpected_field";
    case ErrorCode::kMissingField: return "missing_field";
    case ErrorCode::kDuplicateField: return "duplicate_field";
    case ErrorCode::kUnsupportedVersion: return "unsupported_version";
    case ErrorCode::kInvalidPath: return "invalid_path";
    case ErrorCode::kInvalidFenceMask: return "invalid_fence_mask";
    case ErrorCode::kReservedFieldNotZero: return "reserved_field_not_zero";
    case ErrorCode::kTruncatedEncoding: return "truncated_encoding";
    case ErrorCode::kTrailingBytes: return "trailing_bytes";
    case ErrorCode::kInvalidEncoding: return "invalid_encoding";
    case ErrorCode::kLengthOutOfRange: return "length_out_of_range";
    case ErrorCode::kUnknownEntitlement: return "unknown_entitlement";
    case ErrorCode::kRevisionConflict: return "revision_conflict";
    case ErrorCode::kRequestIdConflict: return "request_id_conflict";
    case ErrorCode::kInvalidTransition: return "invalid_transition";
    case ErrorCode::kInsufficientQuantity: return "insufficient_quantity";
    case ErrorCode::kQuantityExceeded: return "quantity_exceeded";
    case ErrorCode::kUnitMismatch: return "unit_mismatch";
    case ErrorCode::kScopeMismatch: return "scope_mismatch";
    case ErrorCode::kTenantMismatch: return "tenant_mismatch";
    case ErrorCode::kServiceMismatch: return "service_mismatch";
    case ErrorCode::kServiceClassMismatch: return "service_class_mismatch";
    case ErrorCode::kStaleBinding: return "stale_binding";
    case ErrorCode::kStaleEpoch: return "stale_epoch";
    case ErrorCode::kNotYetEffective: return "not_yet_effective";
    case ErrorCode::kExpired: return "expired";
    case ErrorCode::kRevoked: return "revoked";
    case ErrorCode::kSuspended: return "suspended";
    case ErrorCode::kSuperseded: return "superseded";
    case ErrorCode::kArithmeticOverflow: return "arithmetic_overflow";
    case ErrorCode::kArithmeticUnderflow: return "arithmetic_underflow";
    case ErrorCode::kLimitExceeded: return "limit_exceeded";
    case ErrorCode::kMissingAdmissionEvidence: return "missing_admission_evidence";
    case ErrorCode::kLineageViolation: return "lineage_violation";
    case ErrorCode::kConflict: return "conflict";
    case ErrorCode::kSelfReference: return "self_reference";
    case ErrorCode::kNotAuthorizedHolder: return "not_authorized_holder";
    case ErrorCode::kWindowViolation: return "window_violation";
    case ErrorCode::kCapacityExhausted: return "capacity_exhausted";
    case ErrorCode::kDuplicateIdentity: return "duplicate_identity";
    case ErrorCode::kNoRecordedOutcome: return "no_recorded_outcome";
    case ErrorCode::kIoError: return "io_error";
    case ErrorCode::kPathError: return "path_error";
    case ErrorCode::kWriterLockHeld: return "writer_lock_held";
    case ErrorCode::kStoreNotFound: return "store_not_found";
    case ErrorCode::kStoreAlreadyExists: return "store_already_exists";
    case ErrorCode::kCorruptManifest: return "corrupt_manifest";
    case ErrorCode::kCorruptLog: return "corrupt_log";
    case ErrorCode::kCorruptSnapshot: return "corrupt_snapshot";
    case ErrorCode::kIntegrityFailure: return "integrity_failure";
    case ErrorCode::kStateDigestMismatch: return "state_digest_mismatch";
    case ErrorCode::kRollbackDetected: return "rollback_detected";
    case ErrorCode::kReadOnlyStore: return "read_only_store";
    case ErrorCode::kSegmentFull: return "segment_full";
    case ErrorCode::kNotADirectory: return "not_a_directory";
    case ErrorCode::kUnsupportedFormat: return "unsupported_format";
    case ErrorCode::kSnapshotInconsistent: return "snapshot_inconsistent";
    case ErrorCode::kLockError: return "lock_error";
    case ErrorCode::kInternalError: return "internal_error";
  }
  return "unknown_error_code";
}

bool error_code_is_validation(ErrorCode code) noexcept {
  const auto value = static_cast<std::uint16_t>(code);
  return value >= 1u && value <= 29u;
}

bool error_code_is_authority_refusal(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kStaleBinding:
    case ErrorCode::kStaleEpoch:
    case ErrorCode::kNotYetEffective:
    case ErrorCode::kExpired:
    case ErrorCode::kRevoked:
    case ErrorCode::kSuspended:
    case ErrorCode::kSuperseded:
    case ErrorCode::kQuantityExceeded:
    case ErrorCode::kUnknownEntitlement:
      return true;
    default:
      return false;
  }
}

bool error_code_is_durability(ErrorCode code) noexcept {
  const auto value = static_cast<std::uint16_t>(code);
  return value >= 70u && value <= 99u;
}

std::string Error::to_string() const {
  std::string out = code_name();
  out.append(": ");
  out.append(message_.empty() ? "no detail" : message_);
  if (!detail_.empty()) {
    out.append(" (");
    out.append(detail_);
    out.append(")");
  }
  return out;
}

// ---------------------------------------------------------------------------
// IdentifierValue
// ---------------------------------------------------------------------------

Result<IdentifierValue> IdentifierValue::parse(std::string_view text) {
  if (!is_valid_identifier(text)) {
    return Error(ErrorCode::kInvalidIdentifier,
                 "identifier does not satisfy the identifier grammar",
                 text.empty() ? std::string("<empty>") : std::string(truncate_utf8(text, 32)));
  }
  IdentifierValue value;
  std::memcpy(value.data_.data(), text.data(), text.size());
  value.size_ = static_cast<std::uint8_t>(text.size());
  return value;
}

void IdentifierValue::encode(CanonicalWriter& writer) const { writer.text(view()); }

IdentifierValue IdentifierValue::decode(CanonicalReader& reader) {
  const std::string text = reader.text(kMaxIdentifierBytes);
  if (!reader.ok()) {
    return IdentifierValue{};
  }
  auto parsed = IdentifierValue::parse(text);
  if (!parsed) {
    reader.fail();
    return IdentifierValue{};
  }
  return parsed.value();
}

// ---------------------------------------------------------------------------
// Id16
// ---------------------------------------------------------------------------

template <class Tag>
Id16<Tag> Id16<Tag>::derive(std::span<const std::uint8_t> seed) noexcept {
  const Sha256Digest digest = Sha256::hash(seed);
  Id16<Tag> result;
  std::copy(digest.bytes().begin(), digest.bytes().begin() + static_cast<std::ptrdiff_t>(kSize),
            result.bytes_.begin());
  return result;
}

template <class Tag>
Id16<Tag> Id16<Tag>::derive(std::string_view seed) noexcept {
  return derive(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size()));
}

template <class Tag>
std::optional<Id16<Tag>> Id16<Tag>::from_bytes(std::span<const std::uint8_t> bytes) noexcept {
  if (bytes.size() != kSize) {
    return std::nullopt;
  }
  Id16<Tag> result;
  std::copy(bytes.begin(), bytes.end(), result.bytes_.begin());
  return result;
}

template <class Tag>
std::optional<Id16<Tag>> Id16<Tag>::from_hex(std::string_view hex) noexcept {
  const auto decoded = hex_to_bytes(hex, kSize);
  if (!decoded.has_value()) {
    return std::nullopt;
  }
  return from_bytes(std::span<const std::uint8_t>(decoded->data(), decoded->size()));
}

template <class Tag>
std::string Id16<Tag>::to_hex() const {
  return bytes_to_hex(span());
}

template <class Tag>
bool Id16<Tag>::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes_) {
    if (byte != 0u) {
      return false;
    }
  }
  return true;
}

template class Id16<RequestIdTag>;
template class Id16<EntitlementIdTag>;
template class Id16<ReservationIdTag>;
template class Id16<TokenIdTag>;

// ---------------------------------------------------------------------------
// Unit / Quantity
// ---------------------------------------------------------------------------

bool is_valid_unit(Unit unit) noexcept {
  const auto value = static_cast<std::uint8_t>(unit);
  return value >= static_cast<std::uint8_t>(Unit::kCount) &&
         value <= static_cast<std::uint8_t>(Unit::kMilliwatts);
}

const char* unit_name(Unit unit) noexcept {
  switch (unit) {
    case Unit::kCount: return "count";
    case Unit::kBytes: return "bytes";
    case Unit::kMebibytes: return "mib";
    case Unit::kGibibytes: return "gib";
    case Unit::kMebibytesPerSecond: return "mib_per_second";
    case Unit::kMillicores: return "millicores";
    case Unit::kIops: return "iops";
    case Unit::kSessions: return "sessions";
    case Unit::kImages: return "images";
    case Unit::kWatts: return "watts";
    case Unit::kMilliwatts: return "milliwatts";
  }
  return "unknown_unit";
}

std::optional<Unit> unit_from_name(std::string_view name) noexcept {
  if (name == "count" || name == "counts") return Unit::kCount;
  if (name == "bytes" || name == "byte") return Unit::kBytes;
  if (name == "mib" || name == "mebibytes") return Unit::kMebibytes;
  if (name == "gib" || name == "gibibytes") return Unit::kGibibytes;
  if (name == "mib_per_second" || name == "mibps") return Unit::kMebibytesPerSecond;
  if (name == "millicores" || name == "mcpu") return Unit::kMillicores;
  if (name == "iops") return Unit::kIops;
  if (name == "sessions") return Unit::kSessions;
  if (name == "images") return Unit::kImages;
  if (name == "watts") return Unit::kWatts;
  if (name == "milliwatts") return Unit::kMilliwatts;
  return std::nullopt;
}

std::optional<Quantity> Quantity::make(Unit unit, std::uint64_t units) noexcept {
  if (!is_valid_unit(unit)) {
    return std::nullopt;
  }
  Quantity quantity;
  quantity.unit_ = unit;
  quantity.units_ = units;
  return quantity;
}

std::optional<Quantity> Quantity::parse(Unit unit, std::string_view digits) noexcept {
  if (!is_valid_unit(unit) || digits.empty() || digits.size() > 20u) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > ((18446744073709551615ull - digit) / 10ull)) {
      return std::nullopt;
    }
    value = (value * 10ull) + digit;
  }
  return make(unit, value);
}

std::optional<Quantity> Quantity::checked_add(const Quantity& a, const Quantity& b) noexcept {
  if (a.unit_ != b.unit_) {
    return std::nullopt;
  }
  if (a.units_ > (18446744073709551615ull - b.units_)) {
    return std::nullopt;
  }
  return make(a.unit_, a.units_ + b.units_);
}

std::optional<Quantity> Quantity::checked_sub(const Quantity& a, const Quantity& b) noexcept {
  if (a.unit_ != b.unit_ || a.units_ < b.units_) {
    return std::nullopt;
  }
  return make(a.unit_, a.units_ - b.units_);
}

std::string Quantity::to_string() const {
  return std::to_string(units_) + " " + unit_name(unit_);
}

void Quantity::encode(CanonicalWriter& writer) const {
  writer.u8(static_cast<std::uint8_t>(unit_));
  writer.u64(units_);
}

Quantity Quantity::decode(CanonicalReader& reader) {
  const auto raw_unit = reader.u8();
  const std::uint64_t units = reader.u64();
  if (!reader.ok()) {
    return Quantity{};
  }
  const auto unit = static_cast<Unit>(raw_unit);
  auto made = Quantity::make(unit, units);
  if (!made) {
    reader.fail();
    return Quantity{};
  }
  return made.value();
}

// ---------------------------------------------------------------------------
// Priority
// ---------------------------------------------------------------------------

bool is_valid_priority_class(PriorityClass klass) noexcept {
  const auto value = static_cast<std::uint8_t>(klass);
  return value >= static_cast<std::uint8_t>(PriorityClass::kBackground) &&
         value <= static_cast<std::uint8_t>(PriorityClass::kGuaranteed);
}

const char* priority_class_name(PriorityClass klass) noexcept {
  switch (klass) {
    case PriorityClass::kBackground: return "background";
    case PriorityClass::kBestEffort: return "best_effort";
    case PriorityClass::kStandard: return "standard";
    case PriorityClass::kElevated: return "elevated";
    case PriorityClass::kCritical: return "critical";
    case PriorityClass::kGuaranteed: return "guaranteed";
  }
  return "unknown_priority_class";
}

std::optional<PriorityClass> priority_class_from_name(std::string_view name) noexcept {
  if (name == "background") return PriorityClass::kBackground;
  if (name == "best_effort") return PriorityClass::kBestEffort;
  if (name == "standard") return PriorityClass::kStandard;
  if (name == "elevated") return PriorityClass::kElevated;
  if (name == "critical") return PriorityClass::kCritical;
  if (name == "guaranteed") return PriorityClass::kGuaranteed;
  return std::nullopt;
}

void Priority::encode(CanonicalWriter& writer) const {
  writer.u8(static_cast<std::uint8_t>(klass));
  writer.u16(rank);
}

Priority Priority::decode(CanonicalReader& reader) {
  const auto raw_class = reader.u8();
  const std::uint16_t rank = reader.u16();
  if (!reader.ok()) {
    return Priority{};
  }
  const auto klass = static_cast<PriorityClass>(raw_class);
  if (!is_valid_priority_class(klass)) {
    reader.fail();
    return Priority{};
  }
  Priority priority;
  priority.klass = klass;
  priority.rank = rank;
  return priority;
}

int compare_priority(const Priority& a, const Priority& b) noexcept {
  if (a.klass != b.klass) {
    return static_cast<std::uint8_t>(a.klass) > static_cast<std::uint8_t>(b.klass) ? -1 : 1;
  }
  if (a.rank != b.rank) {
    return a.rank < b.rank ? -1 : 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

bool scope_matches(const ResourceScope& a, const ResourceScope& b) noexcept {
  return a.facility == b.facility && a.resource_type == b.resource_type && a.scope == b.scope;
}

// ---------------------------------------------------------------------------
// Lifecycle enums
// ---------------------------------------------------------------------------

bool is_valid_entitlement_state(EntitlementState state) noexcept {
  const auto value = static_cast<std::uint8_t>(state);
  return value >= static_cast<std::uint8_t>(EntitlementState::kActive) &&
         value <= static_cast<std::uint8_t>(EntitlementState::kSuperseded);
}

bool is_terminal_state(EntitlementState state) noexcept {
  return state == EntitlementState::kRevoked || state == EntitlementState::kExpired ||
         state == EntitlementState::kSuperseded;
}

const char* entitlement_state_name(EntitlementState state) noexcept {
  switch (state) {
    case EntitlementState::kActive: return "active";
    case EntitlementState::kSuspended: return "suspended";
    case EntitlementState::kRevoked: return "revoked";
    case EntitlementState::kExpired: return "expired";
    case EntitlementState::kSuperseded: return "superseded";
  }
  return "unknown_state";
}

std::optional<EntitlementState> entitlement_state_from_name(std::string_view name) noexcept {
  if (name == "active") return EntitlementState::kActive;
  if (name == "suspended") return EntitlementState::kSuspended;
  if (name == "revoked") return EntitlementState::kRevoked;
  if (name == "expired") return EntitlementState::kExpired;
  if (name == "superseded") return EntitlementState::kSuperseded;
  return std::nullopt;
}

bool is_valid_lineage_relation(LineageRelation relation) noexcept {
  const auto value = static_cast<std::uint8_t>(relation);
  return value >= static_cast<std::uint8_t>(LineageRelation::kRoot) &&
         value <= static_cast<std::uint8_t>(LineageRelation::kMergedFrom);
}

const char* lineage_relation_name(LineageRelation relation) noexcept {
  switch (relation) {
    case LineageRelation::kRoot: return "root";
    case LineageRelation::kSplitFrom: return "split_from";
    case LineageRelation::kDelegatedFrom: return "delegated_from";
    case LineageRelation::kMovedFrom: return "moved_from";
    case LineageRelation::kReissuedFrom: return "reissued_from";
    case LineageRelation::kMergedFrom: return "merged_from";
  }
  return "unknown_relation";
}

bool is_valid_terminal_reason(TerminalReason reason) noexcept {
  const auto value = static_cast<std::uint8_t>(reason);
  return value <= static_cast<std::uint8_t>(TerminalReason::kParentRevoked);
}

const char* terminal_reason_name(TerminalReason reason) noexcept {
  switch (reason) {
    case TerminalReason::kNotApplicable: return "not_applicable";
    case TerminalReason::kOperatorRevocation: return "operator_revocation";
    case TerminalReason::kPolicyRevocation: return "policy_revocation";
    case TerminalReason::kExpiredByTime: return "expired_by_time";
    case TerminalReason::kSupersededByReissue: return "superseded_by_reissue";
    case TerminalReason::kSupersededByMerge: return "superseded_by_merge";
    case TerminalReason::kParentRevoked: return "parent_revoked";
  }
  return "unknown_terminal_reason";
}

bool is_valid_transfer_mode(TransferMode mode) noexcept {
  const auto value = static_cast<std::uint8_t>(mode);
  return value >= static_cast<std::uint8_t>(TransferMode::kMove) &&
         value <= static_cast<std::uint8_t>(TransferMode::kDelegate);
}

const char* transfer_mode_name(TransferMode mode) noexcept {
  switch (mode) {
    case TransferMode::kMove: return "move";
    case TransferMode::kSplit: return "split";
    case TransferMode::kDelegate: return "delegate";
  }
  return "unknown_transfer_mode";
}

std::optional<TransferMode> transfer_mode_from_name(std::string_view name) noexcept {
  if (name == "move") return TransferMode::kMove;
  if (name == "split") return TransferMode::kSplit;
  if (name == "delegate") return TransferMode::kDelegate;
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Events and commit stages
// ---------------------------------------------------------------------------

const char* event_kind_name(EventKind kind) noexcept {
  switch (kind) {
    case EventKind::kStoreOpened: return "store_opened";
    case EventKind::kAuthorityPublished: return "authority_published";
    case EventKind::kFencedOnOpen: return "fenced_on_open";
    case EventKind::kGranted: return "granted";
    case EventKind::kSuspended: return "suspended";
    case EventKind::kResumed: return "resumed";
    case EventKind::kRevoked: return "revoked";
    case EventKind::kExpired: return "expired";
    case EventKind::kMoved: return "moved";
    case EventKind::kSplit: return "split";
    case EventKind::kDelegated: return "delegated";
    case EventKind::kMerged: return "merged";
    case EventKind::kReissued: return "reissued";
    case EventKind::kDrawn: return "drawn";
    case EventKind::kReleased: return "released";
    case EventKind::kCompacted: return "compacted";
  }
  return "unknown_event";
}

const char* commit_stage_name(CommitStage stage) noexcept {
  switch (stage) {
    case CommitStage::kBeforeAppend: return "before_append";
    case CommitStage::kAfterAppendBeforeFlush: return "after_append_before_flush";
    case CommitStage::kAfterFlushBeforeManifest: return "after_flush_before_manifest";
    case CommitStage::kAfterManifestPublish: return "after_manifest_publish";
    case CommitStage::kAfterSegmentRotateBeforeManifest: return "after_segment_rotate_before_manifest";
  }
  return "unknown_commit_stage";
}

}  // namespace entl
