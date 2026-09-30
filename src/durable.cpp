// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "durable.hpp"

#include "resource_entitlement/version.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>

namespace entl::detail {
namespace {

constexpr std::uint32_t kSegmentIndexLimit = 0xFFFFFFFFu;
constexpr std::size_t kMaxEntitlements = 4000000;
constexpr std::size_t kMaxIdempotencyEntriesRead = 1u << 20;
constexpr std::size_t kMaxPostImagesPerEntry = 65536;

const char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] std::string hex_fixed(std::uint64_t value, int digits) {
  std::string out(static_cast<std::size_t>(digits), '0');
  for (int i = digits - 1; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHexDigits[value & 0xFu];
    value >>= 4;
  }
  return out;
}

[[nodiscard]] bool parse_hex_fixed(std::string_view text, std::size_t digits, std::uint64_t& value) noexcept {
  if (text.size() != digits) {
    return false;
  }
  std::uint64_t parsed = 0;
  for (const char c : text) {
    std::uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint64_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<std::uint64_t>(c - 'A' + 10);
    } else {
      return false;
    }
    parsed = (parsed << 4) | digit;
  }
  value = parsed;
  return true;
}

[[nodiscard]] bool has_prefix_suffix(std::string_view text, std::string_view prefix, std::string_view suffix) {
  return text.size() > (prefix.size() + suffix.size()) &&
         text.compare(0, prefix.size(), prefix) == 0 &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void encode_idempotency(CanonicalWriter& writer, const IdempotencyEntry& entry) {
  entry.request_id.encode(writer);
  entry.outcome.encode(writer);
}

IdempotencyEntry decode_idempotency(CanonicalReader& reader) {
  IdempotencyEntry entry;
  entry.request_id = RequestId::decode(reader);
  entry.outcome = MutationOutcome::decode(reader);
  return entry;
}

void apply_entry(StateCore& state, const JournalEntry& entry) {
  const MutationOutcome& outcome = entry.outcome;
  state.commit_seq = outcome.commit_seq;
  for (const Entitlement& record : entry.post_images) {
    state.insert_or_replace(record);
    if (record.mint_ordinal > state.mint_ordinal) {
      state.mint_ordinal = record.mint_ordinal;
    }
  }
  if (entry.authority_changed) {
    state.authority = entry.authority_after;
  }
  record_idempotency(state, outcome);
}

struct SlotRead {
  bool present{false};
  bool valid{false};
  Manifest manifest{};
  std::string problem{};
};

[[nodiscard]] SlotRead read_slot(const std::filesystem::path& path) {
  SlotRead result;
  if (!path_exists(path)) {
    result.problem = "missing";
    return result;
  }
  result.present = true;
  auto size = file_size_bytes(path);
  if (!size.has_value()) {
    result.problem = "unreadable";
    return result;
  }
  if (size.value() != kManifestFileSize) {
    result.problem = "wrong size (" + std::to_string(size.value()) + ")";
    return result;
  }
  auto bytes = read_file(path, kManifestFileSize);
  if (!bytes.has_value()) {
    result.problem = "unreadable";
    return result;
  }
  auto decoded = Manifest::decode(std::span<const std::uint8_t>(bytes.value().data(), bytes.value().size()));
  if (!decoded.has_value()) {
    result.problem = decoded.error().message();
    return result;
  }
  result.manifest = decoded.value();
  result.valid = true;
  return result;
}

[[nodiscard]] std::string describe_manifest(const Manifest& manifest) {
  return "commit_seq=" + std::to_string(manifest.commit_seq.value()) +
         " snapshot_seq=" + std::to_string(manifest.snapshot_seq.value()) +
         " segment=" + std::to_string(manifest.active_segment) +
         " offset=" + std::to_string(manifest.active_offset);
}

}  // namespace

// ---------------------------------------------------------------------------
// Paths and names
// ---------------------------------------------------------------------------

std::filesystem::path lock_path(const std::filesystem::path& directory) {
  return directory / "entitlement.lock";
}

std::filesystem::path manifest_path(const std::filesystem::path& directory, int slot) {
  return directory / (slot == 0 ? "manifest.a" : "manifest.b");
}

std::filesystem::path fence_path(const std::filesystem::path& directory) { return directory / "fence.bin"; }

std::filesystem::path segment_path(const std::filesystem::path& directory, std::uint32_t index) {
  return directory / ("log-" + hex_fixed(index, 8) + ".bin");
}

std::filesystem::path snapshot_path(const std::filesystem::path& directory, Sequence seq) {
  return directory / ("snapshot-" + hex_fixed(seq.value(), 16) + ".bin");
}

bool parse_segment_name(std::string_view name, std::uint32_t& index) noexcept {
  if (!has_prefix_suffix(name, "log-", ".bin")) {
    return false;
  }
  const std::string_view digits = name.substr(4, name.size() - 8);
  std::uint64_t value = 0;
  if (!parse_hex_fixed(digits, 8, value) || value > kSegmentIndexLimit) {
    return false;
  }
  index = static_cast<std::uint32_t>(value);
  return true;
}

bool parse_snapshot_name(std::string_view name, Sequence& seq) noexcept {
  if (!has_prefix_suffix(name, "snapshot-", ".bin")) {
    return false;
  }
  const std::string_view digits = name.substr(9, name.size() - 13);
  std::uint64_t value = 0;
  if (!parse_hex_fixed(digits, 16, value)) {
    return false;
  }
  seq = Sequence::from_value(value);
  return true;
}

Result<void> validate_options(const StoreOpenOptions& options) {
  if (options.directory.empty()) {
    return Error(ErrorCode::kInvalidArgument, "store directory must not be empty");
  }
  if (options.max_segment_bytes < kMinSegmentBytes || options.max_segment_bytes > kMaxSegmentBytes) {
    return Error(ErrorCode::kInvalidArgument, "max_segment_bytes is outside the supported range",
                 std::to_string(options.max_segment_bytes));
  }
  if (options.max_journal_entry_bytes < kMinJournalEntryBytes ||
      options.max_journal_entry_bytes > kMaxJournalEntryBytes) {
    return Error(ErrorCode::kInvalidArgument, "max_journal_entry_bytes is outside the supported range",
                 std::to_string(options.max_journal_entry_bytes));
  }
  if (options.max_journal_entry_bytes > options.max_segment_bytes) {
    return Error(ErrorCode::kInvalidArgument,
                 "max_journal_entry_bytes must not exceed max_segment_bytes");
  }
  if (options.max_idempotency_entries < kMinIdempotencyCapacity ||
      options.max_idempotency_entries > kMaxIdempotencyCapacity) {
    return Error(ErrorCode::kInvalidArgument, "max_idempotency_entries is outside the supported range",
                 std::to_string(options.max_idempotency_entries));
  }
  if (options.read_only && options.create_if_missing) {
    return Error(ErrorCode::kInvalidArgument, "read_only and create_if_missing are mutually exclusive");
  }
  if (options.read_only && options.fence_live_authority_on_open) {
    return Error(ErrorCode::kInvalidArgument,
                 "read_only and fence_live_authority_on_open are mutually exclusive");
  }
  return {};
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> Manifest::encode() const {
  CanonicalWriter writer;
  writer.u32(kManifestMagic);
  writer.u16(version);
  writer.u16(kind);
  writer.u32(reserved);
  writer.u64(manifest_seq.value());
  writer.u64(commit_seq.value());
  writer.u64(snapshot_seq.value());
  writer.u32(active_segment);
  writer.u32(reserved2);
  writer.u64(active_offset);
  writer.u64(authority_revision.value());
  writer.u64(control_epoch.value());
  writer.u64(mint_ordinal.value());
  writer.u32(idempotency_capacity);
  writer.u32(reserved3);
  writer.fixed(head_digest.span());
  writer.fixed(snapshot_digest.span());
  writer.fixed(prev_manifest_digest.span());
  writer.fixed(head_record_digest.span());

  std::vector<std::uint8_t> out = writer.data();
  out.resize(kManifestFieldsSize, 0u);
  const std::uint32_t checksum = crc32c(std::span<const std::uint8_t>(out.data(), kManifestCrcOffset));
  for (unsigned i = 0; i < 4u; ++i) {
    out.push_back(static_cast<std::uint8_t>((checksum >> (i * 8u)) & 0xFFu));
  }
  const Sha256Digest digest = Sha256::hash(std::span<const std::uint8_t>(out.data(), kManifestDigestOffset));
  out.insert(out.end(), digest.bytes().begin(), digest.bytes().end());
  out.resize(kManifestFileSize, 0u);
  return out;
}

Result<Manifest> Manifest::decode(std::span<const std::uint8_t> bytes) {
  if (bytes.size() != kManifestFileSize) {
    return Error(ErrorCode::kCorruptManifest, "manifest is not exactly the required size");
  }
  for (std::size_t i = kManifestUsedSize; i < kManifestFileSize; ++i) {
    if (bytes[i] != 0u) {
      return Error(ErrorCode::kReservedFieldNotZero, "manifest reserved tail is not zero");
    }
  }
  const std::uint32_t stored_crc = static_cast<std::uint32_t>(bytes[kManifestCrcOffset]) |
                                   (static_cast<std::uint32_t>(bytes[kManifestCrcOffset + 1u]) << 8) |
                                   (static_cast<std::uint32_t>(bytes[kManifestCrcOffset + 2u]) << 16) |
                                   (static_cast<std::uint32_t>(bytes[kManifestCrcOffset + 3u]) << 24);
  if (crc32c(std::span<const std::uint8_t>(bytes.data(), kManifestCrcOffset)) != stored_crc) {
    return Error(ErrorCode::kCorruptManifest, "manifest checksum mismatch");
  }
  const Sha256Digest computed = Sha256::hash(std::span<const std::uint8_t>(bytes.data(), kManifestDigestOffset));
  const auto stored = Sha256Digest::from_bytes(bytes.subspan(kManifestDigestOffset, Sha256Digest::kSize));
  if (!stored.has_value() || stored.value() != computed) {
    return Error(ErrorCode::kCorruptManifest, "manifest digest mismatch");
  }

  Manifest manifest;
  CanonicalReader reader(std::span<const std::uint8_t>(bytes.data(), kManifestFieldsSize));
  if (reader.u32() != kManifestMagic) {
    return Error(ErrorCode::kCorruptManifest, "manifest magic mismatch");
  }
  manifest.version = reader.u16();
  manifest.kind = reader.u16();
  manifest.reserved = reader.u32();
  manifest.manifest_seq = Sequence::from_value(reader.u64());
  manifest.commit_seq = Sequence::from_value(reader.u64());
  manifest.snapshot_seq = Sequence::from_value(reader.u64());
  manifest.active_segment = reader.u32();
  manifest.reserved2 = reader.u32();
  manifest.active_offset = reader.u64();
  manifest.authority_revision = Revision::from_value(reader.u64());
  manifest.control_epoch = Epoch::from_value(reader.u64());
  manifest.mint_ordinal = MintOrdinal::from_value(reader.u64());
  manifest.idempotency_capacity = reader.u32();
  manifest.reserved3 = reader.u32();
  const auto head = reader.fixed(Sha256Digest::kSize);
  const auto snapshot = reader.fixed(Sha256Digest::kSize);
  const auto previous = reader.fixed(Sha256Digest::kSize);
  const auto head_record = reader.fixed(Sha256Digest::kSize);
  if (!reader.ok()) {
    return Error(ErrorCode::kCorruptManifest, "manifest field decoding failed");
  }
  const auto head_digest = Sha256Digest::from_bytes(head);
  const auto snapshot_digest = Sha256Digest::from_bytes(snapshot);
  const auto prev_digest = Sha256Digest::from_bytes(previous);
  const auto record_digest = Sha256Digest::from_bytes(head_record);
  if (!head_digest.has_value() || !snapshot_digest.has_value() || !prev_digest.has_value() ||
      !record_digest.has_value()) {
    return Error(ErrorCode::kCorruptManifest, "manifest digest field has the wrong length");
  }
  manifest.head_digest = head_digest.value();
  manifest.snapshot_digest = snapshot_digest.value();
  manifest.prev_manifest_digest = prev_digest.value();
  manifest.head_record_digest = record_digest.value();
  manifest.manifest_digest = stored.value();

  if (manifest.version != kManifestVersion || manifest.kind != 1) {
    return Error(ErrorCode::kUnsupportedFormat, "manifest version or kind is not supported",
                 "version=" + std::to_string(manifest.version));
  }
  if (manifest.reserved != 0u || manifest.reserved2 != 0u || manifest.reserved3 != 0u) {
    return Error(ErrorCode::kReservedFieldNotZero, "manifest reserved field is not zero");
  }
  if (manifest.idempotency_capacity < kMinIdempotencyCapacity ||
      manifest.idempotency_capacity > kMaxIdempotencyCapacity) {
    return Error(ErrorCode::kCorruptManifest, "manifest idempotency capacity is out of range");
  }
  if (manifest.snapshot_seq > manifest.commit_seq) {
    return Error(ErrorCode::kCorruptManifest, "manifest snapshot sequence is ahead of the commit sequence");
  }
  if (manifest.manifest_seq.is_zero() && (manifest.commit_seq.is_set() || manifest.snapshot_seq.is_set())) {
    return Error(ErrorCode::kCorruptManifest,
                 "the first publication of a store cannot carry a commit or snapshot sequence");
  }
  if (manifest.commit_seq.is_zero() && manifest.active_offset != 0u) {
    return Error(ErrorCode::kCorruptManifest, "empty store cannot have a non-zero active offset");
  }
  if (manifest.commit_seq.is_set() && manifest.active_offset == 0u && manifest.active_segment == 0u) {
    return Error(ErrorCode::kCorruptManifest, "non-empty store cannot have an empty active segment");
  }
  return manifest;
}

// ---------------------------------------------------------------------------
// Fence
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> FenceRecord::encode() const {
  CanonicalWriter writer;
  writer.u32(kFenceMagic);
  writer.u16(version);
  writer.u16(kind);
  writer.u32(reserved);
  writer.u64(high_water_seq.value());
  writer.u64(reserved2);
  std::vector<std::uint8_t> out = writer.data();
  out.resize(kFenceFieldsSize, 0u);
  const std::uint32_t checksum = crc32c(std::span<const std::uint8_t>(out.data(), kFenceCrcOffset));
  for (unsigned i = 0; i < 4u; ++i) {
    out.push_back(static_cast<std::uint8_t>((checksum >> (i * 8u)) & 0xFFu));
  }
  const Sha256Digest digest = Sha256::hash(std::span<const std::uint8_t>(out.data(), kFenceDigestOffset));
  out.insert(out.end(), digest.bytes().begin(), digest.bytes().end());
  out.resize(kFenceSlotSize, 0u);
  return out;
}

Result<FenceRecord> FenceRecord::decode(std::span<const std::uint8_t> bytes) {
  if (bytes.size() != kFenceSlotSize) {
    return Error(ErrorCode::kIntegrityFailure, "fence slot is not exactly the required size");
  }
  for (std::size_t i = kFenceUsedSize; i < kFenceSlotSize; ++i) {
    if (bytes[i] != 0u) {
      return Error(ErrorCode::kIntegrityFailure, "fence reserved tail is not zero");
    }
  }
  const std::uint32_t stored_crc = static_cast<std::uint32_t>(bytes[kFenceCrcOffset]) |
                                   (static_cast<std::uint32_t>(bytes[kFenceCrcOffset + 1u]) << 8) |
                                   (static_cast<std::uint32_t>(bytes[kFenceCrcOffset + 2u]) << 16) |
                                   (static_cast<std::uint32_t>(bytes[kFenceCrcOffset + 3u]) << 24);
  if (crc32c(std::span<const std::uint8_t>(bytes.data(), kFenceCrcOffset)) != stored_crc) {
    return Error(ErrorCode::kIntegrityFailure, "fence checksum mismatch");
  }
  const Sha256Digest computed = Sha256::hash(std::span<const std::uint8_t>(bytes.data(), kFenceDigestOffset));
  const auto stored = Sha256Digest::from_bytes(bytes.subspan(kFenceDigestOffset, Sha256Digest::kSize));
  if (!stored.has_value() || stored.value() != computed) {
    return Error(ErrorCode::kIntegrityFailure, "fence digest mismatch");
  }
  CanonicalReader reader(std::span<const std::uint8_t>(bytes.data(), kFenceFieldsSize));
  if (reader.u32() != kFenceMagic) {
    return Error(ErrorCode::kIntegrityFailure, "fence magic mismatch");
  }
  FenceRecord record;
  record.version = reader.u16();
  record.kind = reader.u16();
  record.reserved = reader.u32();
  record.high_water_seq = Sequence::from_value(reader.u64());
  record.reserved2 = reader.u64();
  if (!reader.ok()) {
    return Error(ErrorCode::kIntegrityFailure, "fence field decoding failed");
  }
  if (record.version != kFenceVersion || record.kind != 1 || record.reserved != 0u || record.reserved2 != 0u) {
    return Error(ErrorCode::kIntegrityFailure, "fence version, kind, or reserved field is invalid");
  }
  return record;
}

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------

bool is_valid_journal_kind(std::uint8_t kind) noexcept {
  return kind >= static_cast<std::uint8_t>(JournalKind::kAuthorityUpdate) &&
         kind <= static_cast<std::uint8_t>(JournalKind::kFenceOnOpen);
}

const char* journal_kind_name(std::uint8_t kind) noexcept {
  switch (static_cast<JournalKind>(kind)) {
    case JournalKind::kAuthorityUpdate: return "authority_update";
    case JournalKind::kGrant: return "grant";
    case JournalKind::kSuspend: return "suspend";
    case JournalKind::kResume: return "resume";
    case JournalKind::kRevoke: return "revoke";
    case JournalKind::kExpire: return "expire";
    case JournalKind::kMove: return "move";
    case JournalKind::kSplit: return "split";
    case JournalKind::kDelegate: return "delegate";
    case JournalKind::kMerge: return "merge";
    case JournalKind::kReissue: return "reissue";
    case JournalKind::kDraw: return "draw";
    case JournalKind::kRelease: return "release";
    case JournalKind::kFenceOnOpen: return "fence_on_open";
  }
  return "unknown_journal_kind";
}

std::vector<std::uint8_t> JournalEntry::encode() const {
  CanonicalWriter writer;
  writer.u16(kJournalEntryVersion);
  writer.u8(kind);
  writer.boolean(authority_changed);
  writer.u8(0);
  outcome.encode(writer);
  writer.u32(static_cast<std::uint32_t>(post_images.size()));
  for (const Entitlement& record : post_images) {
    record.encode(writer);
  }
  authority_after.encode(writer);
  return writer.data();
}

Result<JournalEntry> JournalEntry::decode(std::span<const std::uint8_t> bytes) {
  JournalEntry entry;
  CanonicalReader reader(bytes);
  const std::uint16_t version = reader.u16();
  entry.kind = reader.u8();
  entry.authority_changed = reader.boolean();
  const std::uint8_t reserved = reader.u8();
  entry.outcome = MutationOutcome::decode(reader);
  const std::uint32_t count = reader.u32();
  if (!reader.ok()) {
    return Error(ErrorCode::kCorruptLog, "journal entry header could not be decoded");
  }
  if (version != kJournalEntryVersion) {
    return Error(ErrorCode::kUnsupportedFormat, "journal entry version is not supported",
                 std::to_string(version));
  }
  if (!is_valid_journal_kind(entry.kind)) {
    return Error(ErrorCode::kCorruptLog, "journal entry kind is not a known mutation kind");
  }
  if (reserved != 0u) {
    return Error(ErrorCode::kReservedFieldNotZero, "journal entry reserved byte is not zero");
  }
  if (static_cast<std::size_t>(count) > kMaxPostImagesPerEntry) {
    return Error(ErrorCode::kCorruptLog, "journal entry declares too many post-image records");
  }
  entry.post_images.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    entry.post_images.push_back(Entitlement::decode(reader));
  }
  entry.authority_after = AuthoritySnapshot::decode(reader);
  auto finished = reader.finish();
  if (!finished) {
    return Error(ErrorCode::kCorruptLog, "journal entry payload is malformed", finished.error().message());
  }
  if (entry.outcome.code != ErrorCode::kOk) {
    return Error(ErrorCode::kCorruptLog, "a committed journal entry must record an accepted outcome");
  }
  return entry;
}

std::vector<std::uint8_t> encode_log_record(const LogRecordHeader& header, std::span<const std::uint8_t> payload) {
  CanonicalWriter writer;
  writer.u32(kLogRecordMagic);
  writer.u16(header.version);
  writer.u8(header.kind);
  writer.u8(header.reserved0);
  writer.u32(header.flags);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.u64(header.seq.value());
  writer.fixed(header.prev_record_digest.span());
  writer.fixed(Sha256::hash(payload).span());
  writer.fixed(payload);
  const std::uint32_t checksum = crc32c(writer.span());
  writer.u32(checksum);
  return writer.data();
}

Result<DecodedLogRecord> decode_log_record(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kLogRecordHeaderSize) {
    return Error(ErrorCode::kTruncatedEncoding, "log record header is incomplete");
  }
  CanonicalReader reader(bytes);
  if (reader.u32() != kLogRecordMagic) {
    return Error(ErrorCode::kCorruptLog, "log record magic mismatch");
  }
  DecodedLogRecord record;
  record.header.version = reader.u16();
  record.header.kind = reader.u8();
  record.header.reserved0 = reader.u8();
  record.header.flags = reader.u32();
  const std::uint32_t payload_length = reader.u32();
  record.header.seq = Sequence::from_value(reader.u64());
  const auto previous = reader.fixed(Sha256Digest::kSize);
  const auto payload_digest = reader.fixed(Sha256Digest::kSize);
  if (!reader.ok()) {
    return Error(ErrorCode::kCorruptLog, "log record header could not be decoded");
  }
  const auto prev_digest = Sha256Digest::from_bytes(previous);
  const auto payload_digest_value = Sha256Digest::from_bytes(payload_digest);
  if (!prev_digest.has_value() || !payload_digest_value.has_value()) {
    return Error(ErrorCode::kCorruptLog, "log record digest field has the wrong length");
  }
  record.header.prev_record_digest = prev_digest.value();
  record.header.payload_digest = payload_digest_value.value();

  if (record.header.version != kLogRecordVersion) {
    return Error(ErrorCode::kUnsupportedFormat, "log record version is not supported",
                 std::to_string(record.header.version));
  }
  if (record.header.reserved0 != 0u || record.header.flags != 0u) {
    return Error(ErrorCode::kCorruptLog, "log record reserved fields are not zero");
  }
  if (!is_valid_journal_kind(record.header.kind)) {
    return Error(ErrorCode::kCorruptLog, "log record kind is not a known mutation kind");
  }
  if (payload_length > kMaxJournalEntryBytes) {
    return Error(ErrorCode::kCorruptLog, "log record declares a payload beyond the supported bound");
  }
  const std::size_t total = kLogRecordHeaderSize + static_cast<std::size_t>(payload_length) +
                            kLogRecordTrailerSize;
  if (bytes.size() < total) {
    return Error(ErrorCode::kTruncatedEncoding, "log record payload is incomplete");
  }
  const std::span<const std::uint8_t> covered = bytes.subspan(0, kLogRecordHeaderSize + payload_length);
  const std::uint32_t stored_crc = static_cast<std::uint32_t>(bytes[kLogRecordHeaderSize + payload_length]) |
                                   (static_cast<std::uint32_t>(bytes[kLogRecordHeaderSize + payload_length + 1u]) << 8) |
                                   (static_cast<std::uint32_t>(bytes[kLogRecordHeaderSize + payload_length + 2u]) << 16) |
                                   (static_cast<std::uint32_t>(bytes[kLogRecordHeaderSize + payload_length + 3u]) << 24);
  if (crc32c(covered) != stored_crc) {
    return Error(ErrorCode::kCorruptLog, "log record checksum mismatch");
  }
  record.payload = covered.subspan(kLogRecordHeaderSize);
  if (Sha256::hash(record.payload) != record.header.payload_digest) {
    return Error(ErrorCode::kCorruptLog, "log record payload digest mismatch");
  }
  record.header.record_digest = Sha256::hash(covered);
  record.total_length = total;
  return record;
}

// ---------------------------------------------------------------------------
// StateCore
// ---------------------------------------------------------------------------

void record_idempotency(StateCore& state, const MutationOutcome& outcome) {
  IdempotencyEntry entry;
  entry.request_id = outcome.request_id;
  entry.outcome = outcome;
  state.idempotency.push_back(entry);
  while (state.idempotency.size() > static_cast<std::size_t>(state.idempotency_capacity)) {
    state.idempotency.erase(state.idempotency.begin());
  }
}

void StateCore::sort_entitlements() {
  std::sort(entitlements.begin(), entitlements.end(),
            [](const Entitlement& a, const Entitlement& b) { return a.id < b.id; });
}

const Entitlement* StateCore::find(const EntitlementId& id) const {
  const auto it = std::lower_bound(entitlements.begin(), entitlements.end(), id,
                                   [](const Entitlement& record, const EntitlementId& key) {
                                     return record.id < key;
                                   });
  if (it == entitlements.end() || !(it->id == id)) {
    return nullptr;
  }
  return &(*it);
}

Entitlement* StateCore::find(const EntitlementId& id) {
  const auto it = std::lower_bound(entitlements.begin(), entitlements.end(), id,
                                   [](const Entitlement& record, const EntitlementId& key) {
                                     return record.id < key;
                                   });
  if (it == entitlements.end() || !(it->id == id)) {
    return nullptr;
  }
  return &(*it);
}

void StateCore::insert_or_replace(const Entitlement& record) {
  const auto it = std::lower_bound(entitlements.begin(), entitlements.end(), record.id,
                                   [](const Entitlement& existing, const EntitlementId& key) {
                                     return existing.id < key;
                                   });
  if (it != entitlements.end() && it->id == record.id) {
    *it = record;
    return;
  }
  entitlements.insert(it, record);
}

std::vector<std::uint8_t> StateCore::encode() const {
  CanonicalWriter writer;
  writer.u16(kStateVersion);
  writer.u32(static_cast<std::uint32_t>(entitlements.size()));
  for (const Entitlement& record : entitlements) {
    record.encode(writer);
  }
  authority.encode(writer);
  writer.u64(mint_ordinal.value());
  writer.u32(idempotency_capacity);
  writer.u32(static_cast<std::uint32_t>(idempotency.size()));
  for (const IdempotencyEntry& entry : idempotency) {
    encode_idempotency(writer, entry);
  }
  writer.u64(commit_seq.value());
  return writer.data();
}

Result<StateCore> StateCore::decode(std::span<const std::uint8_t> bytes) {
  StateCore state;
  CanonicalReader reader(bytes);
  const std::uint16_t version = reader.u16();
  const std::uint32_t count = reader.u32();
  if (!reader.ok()) {
    return Error(ErrorCode::kCorruptSnapshot, "state header could not be decoded");
  }
  if (version != kStateVersion) {
    return Error(ErrorCode::kUnsupportedFormat, "state version is not supported", std::to_string(version));
  }
  if (static_cast<std::size_t>(count) > kMaxEntitlements) {
    return Error(ErrorCode::kCorruptSnapshot, "state declares too many entitlement records");
  }
  state.entitlements.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    state.entitlements.push_back(Entitlement::decode(reader));
  }
  state.authority = AuthoritySnapshot::decode(reader);
  state.mint_ordinal = MintOrdinal::from_value(reader.u64());
  state.idempotency_capacity = reader.u32();
  const std::uint32_t idempotency_count = reader.u32();
  if (!reader.ok()) {
    return Error(ErrorCode::kCorruptSnapshot, "state body could not be decoded");
  }
  if (state.idempotency_capacity < kMinIdempotencyCapacity ||
      state.idempotency_capacity > kMaxIdempotencyCapacity) {
    return Error(ErrorCode::kCorruptSnapshot, "state idempotency capacity is out of range");
  }
  if (static_cast<std::size_t>(idempotency_count) > kMaxIdempotencyEntriesRead ||
      idempotency_count > state.idempotency_capacity) {
    return Error(ErrorCode::kCorruptSnapshot, "state declares an impossible idempotency entry count");
  }
  state.idempotency.reserve(idempotency_count);
  for (std::uint32_t i = 0; i < idempotency_count; ++i) {
    state.idempotency.push_back(decode_idempotency(reader));
  }
  state.commit_seq = Sequence::from_value(reader.u64());
  auto finished = reader.finish();
  if (!finished) {
    return Error(ErrorCode::kCorruptSnapshot, "state payload is malformed", finished.error().message());
  }
  for (std::size_t i = 1; i < state.entitlements.size(); ++i) {
    if (!(state.entitlements[i - 1].id < state.entitlements[i].id)) {
      return Error(ErrorCode::kCorruptSnapshot, "state entitlement records are not strictly ordered by identity");
    }
  }
  return state;
}

Sha256Digest StateCore::state_digest() const {
  const std::vector<std::uint8_t> bytes = encode();
  return Sha256::hash(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

// ---------------------------------------------------------------------------
// Snapshot framing
// ---------------------------------------------------------------------------
namespace {

std::vector<std::uint8_t> encode_snapshot(const StateCore& state) {
  const std::vector<std::uint8_t> payload = state.encode();
  CanonicalWriter writer;
  writer.u32(kSnapshotMagic);
  writer.u16(kSnapshotVersion);
  writer.u16(1);
  writer.u32(0);
  writer.u64(static_cast<std::uint64_t>(payload.size()));
  writer.fixed(payload);
  const std::uint32_t checksum = crc32c(writer.span());
  writer.u32(checksum);
  const Sha256Digest digest = Sha256::hash(writer.span());
  writer.fixed(digest.span());
  return writer.data();
}

Result<StateCore> decode_snapshot(std::span<const std::uint8_t> bytes) {
  constexpr std::size_t kHeaderSize = 4u + 2u + 2u + 4u + 8u;
  if (bytes.size() < kHeaderSize + kLogRecordTrailerSize + Sha256Digest::kSize) {
    return Error(ErrorCode::kCorruptSnapshot, "snapshot file is shorter than its framing");
  }
  CanonicalReader header(bytes.first(kHeaderSize));
  if (header.u32() != kSnapshotMagic) {
    return Error(ErrorCode::kCorruptSnapshot, "snapshot magic mismatch");
  }
  const std::uint16_t version = header.u16();
  const std::uint16_t kind = header.u16();
  const std::uint32_t reserved = header.u32();
  const std::uint64_t payload_length = header.u64();
  if (!header.ok()) {
    return Error(ErrorCode::kCorruptSnapshot, "snapshot header could not be decoded");
  }
  if (version != kSnapshotVersion || kind != 1 || reserved != 0u) {
    return Error(ErrorCode::kUnsupportedFormat, "snapshot version, kind, or reserved field is invalid");
  }
  if (payload_length > kMaxSnapshotBytes) {
    return Error(ErrorCode::kLimitExceeded, "snapshot declares a payload beyond the supported bound");
  }
  const std::size_t expected =
      kHeaderSize + static_cast<std::size_t>(payload_length) + kLogRecordTrailerSize + Sha256Digest::kSize;
  if (bytes.size() != expected) {
    return Error(ErrorCode::kCorruptSnapshot, "snapshot length does not match its declared payload");
  }
  const std::size_t digest_offset = kHeaderSize + static_cast<std::size_t>(payload_length) +
                                    kLogRecordTrailerSize;
  const std::span<const std::uint8_t> covered = bytes.first(digest_offset);
  const std::uint32_t stored_crc = static_cast<std::uint32_t>(bytes[digest_offset - 4u]) |
                                   (static_cast<std::uint32_t>(bytes[digest_offset - 3u]) << 8) |
                                   (static_cast<std::uint32_t>(bytes[digest_offset - 2u]) << 16) |
                                   (static_cast<std::uint32_t>(bytes[digest_offset - 1u]) << 24);
  if (crc32c(covered.first(digest_offset - 4u)) != stored_crc) {
    return Error(ErrorCode::kCorruptSnapshot, "snapshot checksum mismatch");
  }
  const Sha256Digest computed = Sha256::hash(covered);
  const auto stored = Sha256Digest::from_bytes(bytes.subspan(digest_offset, Sha256Digest::kSize));
  if (!stored.has_value() || stored.value() != computed) {
    return Error(ErrorCode::kCorruptSnapshot, "snapshot digest mismatch");
  }
  return StateCore::decode(bytes.subspan(kHeaderSize, static_cast<std::size_t>(payload_length)));
}

struct Recovery {
  StateCore state{};
  InspectionReport report{};
  Manifest manifest{};
  Sha256Digest head_record_digest{};
  std::int64_t active_offset{0};
  int active_slot{0};
  bool fresh{false};
  Sequence fence_high_water{};
  std::vector<std::uint32_t> segments{};
  std::vector<Sequence> snapshots{};
};

void add_finding(InspectionReport& report, std::string text) {
  if (report.findings.size() < 64u) {
    report.findings.push_back(std::move(text));
  }
}

Result<Recovery> recover_directory(const std::filesystem::path& directory, const StoreOpenOptions& options,
                                   bool writer_mode) {
  Recovery recovery;
  InspectionReport& report = recovery.report;
  report.format_version = kManifestVersion;

  const SlotRead slot_a = read_slot(manifest_path(directory, 0));
  const SlotRead slot_b = read_slot(manifest_path(directory, 1));

  bool have_manifest = false;
  const SlotRead* other = nullptr;
  if (slot_a.valid && slot_b.valid) {
    report.manifest_slots_consistent = true;
    if (slot_a.manifest.manifest_seq == slot_b.manifest.manifest_seq) {
      if (slot_a.manifest.manifest_digest != slot_b.manifest.manifest_digest) {
        return Error(ErrorCode::kCorruptManifest,
                     "both manifest slots are valid, share a publication counter, and disagree");
      }
      recovery.manifest = slot_a.manifest;
      recovery.active_slot = 0;
      other = &slot_b;
    } else if (slot_b.manifest.manifest_seq > slot_a.manifest.manifest_seq) {
      recovery.manifest = slot_b.manifest;
      recovery.active_slot = 1;
      other = &slot_a;
    } else {
      recovery.manifest = slot_a.manifest;
      recovery.active_slot = 0;
      other = &slot_b;
    }
    have_manifest = true;
  } else if (slot_a.valid) {
    recovery.manifest = slot_a.manifest;
    recovery.active_slot = 0;
    other = &slot_b;
    have_manifest = true;
    add_finding(report, "manifest slot B is unusable (" + slot_b.problem + "); recovered from slot A");
  } else if (slot_b.valid) {
    recovery.manifest = slot_b.manifest;
    recovery.active_slot = 1;
    other = &slot_a;
    have_manifest = true;
    add_finding(report, "manifest slot A is unusable (" + slot_a.problem + "); recovered from slot B");
  }

  // Enumerate the directory so that "no durable state at all" can be
  // distinguished from "durable state that cannot be verified".
  auto entries = list_directory(directory);
  if (!entries.has_value()) {
    return entries.error();
  }
  for (const std::filesystem::path& entry : entries.value()) {
    const std::string name = entry.filename().string();
    std::uint32_t index = 0;
    if (parse_segment_name(name, index)) {
      recovery.segments.push_back(index);
      continue;
    }
    Sequence seq{};
    if (parse_snapshot_name(name, seq)) {
      recovery.snapshots.push_back(seq);
    }
  }
  std::sort(recovery.segments.begin(), recovery.segments.end());
  std::sort(recovery.snapshots.begin(), recovery.snapshots.end());

  const bool fence_present = path_exists(fence_path(directory));
  const bool any_artifact = slot_a.present || slot_b.present || fence_present || !recovery.segments.empty() ||
                            !recovery.snapshots.empty();

  if (!have_manifest) {
    if (any_artifact) {
      return Error(ErrorCode::kCorruptManifest,
                   "store directory holds durable artifacts but neither manifest slot is usable",
                   "slotA: " + slot_a.problem + "; slotB: " + slot_b.problem);
    }
    if (!writer_mode || !options.create_if_missing) {
      return Error(ErrorCode::kStoreNotFound, "no entitlement store exists at this directory");
    }
    recovery.fresh = true;
    return recovery;
  }

  report.commit_seq = recovery.manifest.commit_seq;
  report.snapshot_seq = recovery.manifest.snapshot_seq;
  report.state_digest = recovery.manifest.head_digest;
  report.manifest_digest = recovery.manifest.manifest_digest;

  if (other != nullptr && other->valid &&
      other->manifest.manifest_seq.value() + 1u == recovery.manifest.manifest_seq.value()) {
    if (other->manifest.manifest_digest != recovery.manifest.prev_manifest_digest) {
      return Error(ErrorCode::kIntegrityFailure,
                   "the previous manifest slot does not match the recorded predecessor digest");
    }
  }
  if (recovery.manifest.prev_manifest_digest.is_zero() && recovery.manifest.manifest_seq.is_set()) {
    add_finding(report, "manifest carries no predecessor digest (first commit of the store)");
  }

  // Rollback witness.
  Sequence fence_high_water{};
  if (fence_present) {
    auto fence_bytes = read_file(fence_path(directory), kFenceFileSize);
    if (!fence_bytes.has_value()) {
      return fence_bytes.error();
    }
    if (fence_bytes.value().size() != kFenceFileSize) {
      return Error(ErrorCode::kIntegrityFailure, "fence file has the wrong size and cannot be trusted");
    }
    FenceRecord best{};
    bool have_fence = false;
    for (int slot = 0; slot < 2; ++slot) {
      const auto raw = std::span<const std::uint8_t>(fence_bytes.value().data(), fence_bytes.value().size())
                           .subspan(static_cast<std::size_t>(slot) * kFenceSlotSize, kFenceSlotSize);
      auto decoded = FenceRecord::decode(raw);
      if (!decoded.has_value()) {
        continue;
      }
      if (!have_fence || decoded.value().high_water_seq > best.high_water_seq) {
        best = decoded.value();
        have_fence = true;
      }
    }
    if (!have_fence) {
      return Error(ErrorCode::kIntegrityFailure,
                   "neither fence slot can be verified; refusing to guess the rollback high-water mark");
    }
    fence_high_water = best.high_water_seq;
  } else {
    add_finding(report, "fence file is absent; rollback detection is degraded until it is recreated");
  }
  if (fence_high_water > recovery.manifest.commit_seq) {
    report.rollback_detected = true;
    return Error(ErrorCode::kRollbackDetected,
                 "the rollback fence records a higher commit sequence than the manifest",
                 "fence=" + std::to_string(fence_high_water.value()) +
                     " manifest=" + std::to_string(recovery.manifest.commit_seq.value()));
  }

  // Snapshot.
  bool have_snapshot = false;
  if (recovery.manifest.snapshot_seq.is_set()) {
    const std::filesystem::path path = snapshot_path(directory, recovery.manifest.snapshot_seq);
    if (!path_exists(path)) {
      add_finding(report, "the snapshot named by the manifest is missing; falling back to full journal replay");
    } else {
      auto raw = read_file(path, kMaxSnapshotBytes + 4096u);
      if (!raw.has_value()) {
        return raw.error();
      }
      report.bytes_scanned += raw.value().size();
      auto decoded = decode_snapshot(std::span<const std::uint8_t>(raw.value().data(), raw.value().size()));
      if (!decoded.has_value()) {
        return decoded.error();
      }
      auto state = std::move(decoded.value());
      if (state.commit_seq != recovery.manifest.snapshot_seq) {
        return Error(ErrorCode::kSnapshotInconsistent,
                     "snapshot commit sequence does not match the manifest snapshot sequence");
      }
      if (state.idempotency_capacity != recovery.manifest.idempotency_capacity) {
        return Error(ErrorCode::kSnapshotInconsistent,
                     "snapshot idempotency capacity does not match the manifest");
      }
      const Sha256Digest digest = state.state_digest();
      if (digest != recovery.manifest.snapshot_digest) {
        return Error(ErrorCode::kStateDigestMismatch, "snapshot state digest does not match the manifest");
      }
      recovery.state = std::move(state);
      have_snapshot = true;
    }
  }

  if (!have_snapshot) {
    recovery.state = StateCore{};
    recovery.state.idempotency_capacity = recovery.manifest.idempotency_capacity;
    // Without a snapshot the state is rebuilt from the journal alone. The
    // authority snapshot's generation values can only be recovered from the
    // journal, but its revision, control epoch, and mint ordinal are carried by
    // the manifest and must be seeded here or the rebuilt state would start
    // from an empty authority and fail its own digest check.
    recovery.state.authority.revision = recovery.manifest.authority_revision;
    recovery.state.authority.control_epoch = recovery.manifest.control_epoch;
    recovery.state.mint_ordinal = recovery.manifest.mint_ordinal;
    if (recovery.manifest.snapshot_seq.is_set() && recovery.segments.empty()) {
      return Error(ErrorCode::kCorruptSnapshot,
                   "the journal that would rebuild the state has been reclaimed and the snapshot is unusable");
    }
  }

  // Replay.
  const Sequence baseline = have_snapshot ? recovery.manifest.snapshot_seq : Sequence{};
  bool started = false;
  bool have_last_seen = false;
  Sequence last_seen{};
  Sha256Digest last_seen_digest{};
  bool torn_tail = false;
  std::uint64_t active_committed_bytes = 0;

  for (const std::uint32_t index : recovery.segments) {
    const bool is_active = (index == recovery.manifest.active_segment);
    if (index > recovery.manifest.active_segment) {
      add_finding(report, "orphan journal segment " + std::to_string(index) + " beyond the active segment");
      continue;
    }
    const std::filesystem::path path = segment_path(directory, index);
    const std::uint64_t bound = static_cast<std::uint64_t>(kMaxSegmentBytes) +
                                static_cast<std::uint64_t>(kMaxJournalEntryBytes) + 4096u;
    auto raw = read_file(path, bound);
    if (!raw.has_value()) {
      return raw.error();
    }
    report.segments_scanned += 1u;
    report.bytes_scanned += raw.value().size();
    const std::span<const std::uint8_t> bytes(raw.value().data(), raw.value().size());
    // Only the bytes the manifest has published are committed. Anything past
    // them belongs to an interrupted commit and must be ignored, not decoded.
    std::size_t limit = bytes.size();
    if (is_active) {
      if (recovery.manifest.active_offset > bytes.size()) {
        return Error(ErrorCode::kCorruptLog,
                     "the active segment is shorter than the manifest's committed offset",
                     "file=" + std::to_string(bytes.size()) +
                         " committed=" + std::to_string(recovery.manifest.active_offset));
      }
      limit = static_cast<std::size_t>(recovery.manifest.active_offset);
      if (bytes.size() > limit) {
        torn_tail = true;
      }
    }
    const std::span<const std::uint8_t> committed = bytes.first(limit);
    std::size_t offset = 0;
    std::size_t committed_end = 0;
    while (offset < committed.size()) {
      auto decoded = decode_log_record(committed.subspan(offset));
      if (!decoded.has_value()) {
        if (decoded.error().code() == ErrorCode::kTruncatedEncoding) {
          torn_tail = true;
          break;
        }
        return decoded.error();
      }
      const DecodedLogRecord& record = decoded.value();
      if (record.header.seq > recovery.manifest.commit_seq) {
        torn_tail = true;
        break;
      }
      if (have_last_seen && record.header.seq.value() != last_seen.value() + 1u) {
        return Error(ErrorCode::kCorruptLog, "journal record sequence is not contiguous",
                     "expected=" + std::to_string(last_seen.value() + 1u) +
                         " found=" + std::to_string(record.header.seq.value()));
      }
      if (!have_last_seen) {
        // Segments wholly covered by the snapshot may have been reclaimed, so
        // the scan may legitimately begin above sequence 1; it may never begin
        // above the first record the snapshot could still require.
        const std::uint64_t allowed_first = baseline.is_zero() ? 1u : baseline.value() + 1u;
        if (record.header.seq.value() > allowed_first) {
          return Error(ErrorCode::kCorruptLog,
                       "the journal does not begin at the first sequence the snapshot still requires",
                       "found=" + std::to_string(record.header.seq.value()) +
                           " allowed=" + std::to_string(allowed_first));
        }
        if (record.header.seq.value() == 1u && !record.header.prev_record_digest.is_zero()) {
          return Error(ErrorCode::kCorruptLog, "the first journal record must not chain to a predecessor");
        }
      }
      if (have_last_seen && record.header.prev_record_digest != last_seen_digest) {
        return Error(ErrorCode::kCorruptLog, "journal record chain digest does not match its predecessor");
      }
      have_last_seen = true;
      last_seen = record.header.seq;
      last_seen_digest = record.header.record_digest;
      offset += record.total_length;
      committed_end = offset;
      if (record.header.seq <= baseline) {
        continue;
      }
      auto entry = JournalEntry::decode(record.payload);
      if (!entry.has_value()) {
        return entry.error();
      }
      if (entry.value().kind != record.header.kind) {
        return Error(ErrorCode::kCorruptLog, "log record kind disagrees with its payload kind");
      }
      if (entry.value().outcome.commit_seq != record.header.seq) {
        return Error(ErrorCode::kCorruptLog, "log record sequence disagrees with its payload commit sequence");
      }
      apply_entry(recovery.state, entry.value());
      report.records_replayed += 1u;
      started = true;
    }
    if (is_active) {
      active_committed_bytes = committed_end;
    }
  }

  report.torn_tail_discarded = torn_tail;
  if (!recovery.manifest.commit_seq.is_zero()) {
    if (have_last_seen) {
      if (last_seen != recovery.manifest.commit_seq) {
        return Error(ErrorCode::kCorruptLog, "journal replay did not reach the committed sequence",
                     "reached=" + std::to_string(last_seen.value()) +
                         " expected=" + std::to_string(recovery.manifest.commit_seq.value()));
      }
      if (last_seen_digest != recovery.manifest.head_record_digest) {
        return Error(ErrorCode::kIntegrityFailure,
                     "the final journal record digest does not match the manifest");
      }
    } else if (baseline != recovery.manifest.commit_seq) {
      // Nothing was readable and the snapshot does not already cover the
      // committed sequence, so the state cannot be rebuilt at all.
      return Error(ErrorCode::kCorruptLog,
                   "the committed journal records required to rebuild the state were not found");
    }
  }
  (void)started;
  if (active_committed_bytes != recovery.manifest.active_offset) {
    return Error(ErrorCode::kCorruptLog, "the active segment length does not match the committed offset",
                 "found=" + std::to_string(active_committed_bytes) +
                     " expected=" + std::to_string(recovery.manifest.active_offset));
  }

  recovery.state.idempotency_capacity = recovery.manifest.idempotency_capacity;
  recovery.state.commit_seq = recovery.manifest.commit_seq;
  if (recovery.state.authority.revision != recovery.manifest.authority_revision) {
    return Error(ErrorCode::kStateDigestMismatch, "recovered authority revision does not match the manifest");
  }
  if (recovery.state.authority.control_epoch != recovery.manifest.control_epoch) {
    return Error(ErrorCode::kStateDigestMismatch, "recovered control epoch does not match the manifest");
  }
  if (recovery.state.mint_ordinal != recovery.manifest.mint_ordinal) {
    return Error(ErrorCode::kStateDigestMismatch, "recovered mint ordinal does not match the manifest");
  }
  const Sha256Digest digest = recovery.state.state_digest();
  if (digest != recovery.manifest.head_digest) {
    return Error(ErrorCode::kStateDigestMismatch, "recovered state digest does not match the manifest head digest");
  }

  recovery.head_record_digest = recovery.manifest.head_record_digest;
  recovery.fence_high_water = fence_high_water;
  recovery.active_offset = static_cast<std::int64_t>(recovery.manifest.active_offset);
  report.entitlements = recovery.state.entitlements.size();
  report.opened = true;
  for (const std::uint32_t index : recovery.segments) {
    if (index > recovery.manifest.active_segment) {
      report.orphan_records += 1u;
    }
  }

  if (writer_mode) {
    auto entries_writer = list_directory(directory);
    if (!entries_writer.has_value()) {
      return entries_writer.error();
    }
    for (const std::filesystem::path& entry : entries_writer.value()) {
      const std::string name = entry.filename().string();
      if (name.find(".tmp-") != std::string::npos) {
        (void)remove_file_if_exists(directory / entry.filename());
        continue;
      }
      std::uint32_t index = 0;
      if (parse_segment_name(name, index) && index > recovery.manifest.active_segment) {
        (void)remove_file_if_exists(directory / entry.filename());
        continue;
      }
      Sequence seq{};
      if (parse_snapshot_name(name, seq) && seq != recovery.manifest.snapshot_seq) {
        (void)remove_file_if_exists(directory / entry.filename());
      }
    }
    auto active = RandomAccessFile::open(segment_path(directory, recovery.manifest.active_segment), true);
    if (!active.has_value()) {
      return active.error();
    }
    auto actual_size = active.value().size();
    if (!actual_size.has_value()) {
      return actual_size.error();
    }
    if (actual_size.value() != recovery.manifest.active_offset) {
      auto truncated = active.value().truncate(recovery.manifest.active_offset);
      if (!truncated.has_value()) {
        return truncated.error();
      }
      auto flushed = active.value().flush();
      if (!flushed.has_value()) {
        return flushed.error();
      }
      add_finding(report, "uncommitted journal tail was truncated at open");
    }
    if (!fence_present || fence_high_water < recovery.manifest.commit_seq) {
      FenceRecord record;
      record.high_water_seq = recovery.manifest.commit_seq;
      const std::vector<std::uint8_t> bytes = record.encode();
      auto file = RandomAccessFile::open(fence_path(directory), true);
      if (!file.has_value()) {
        return file.error();
      }
      auto written = file.value().write_at(static_cast<std::uint64_t>(0), bytes);
      if (!written.has_value()) {
        return written.error();
      }
      (void)file.value().flush();
    }
  }

  return recovery;
}

/// Builds the state a fresh store starts from: an explicit bootstrap authority
/// snapshot with revision 1 and control epoch 1 and no published generations.
StateCore bootstrap_state(const StoreOpenOptions& options) {
  StateCore state;
  state.authority.revision = Revision::from_value(1u);
  state.authority.control_epoch = Epoch::from_value(1u);
  state.idempotency_capacity = options.max_idempotency_entries;
  state.commit_seq = Sequence{};
  return state;
}

}  // namespace

// ---------------------------------------------------------------------------
// DurableStore
// ---------------------------------------------------------------------------

DurableStore::DurableStore() = default;

DurableStore::~DurableStore() = default;

Result<DurableOpenResult> DurableStore::open(const StoreOpenOptions& options) {
  auto valid = validate_options(options);
  if (!valid) {
    return valid.error();
  }
  auto directory_ready = ensure_directory(options.directory);
  if (!directory_ready) {
    return directory_ready.error();
  }
  auto lock = WriterLock::acquire(lock_path(options.directory));
  if (!lock) {
    return lock.error();
  }

  auto recovery = recover_directory(options.directory, options, true);
  if (!recovery) {
    return recovery.error();
  }

  DurableOpenResult result;
  result.report = recovery.value().report;
  result.report.writer_lock_free = true;

  auto store = std::make_unique<DurableStore>();
  store->directory_ = options.directory;
  store->options_ = options;
  store->lock_ = std::move(lock.value());
  store->read_only_ = false;

  if (recovery.value().fresh) {
    StateCore state = bootstrap_state(options);
    Manifest manifest;
    manifest.version = kManifestVersion;
    manifest.kind = 1;
    manifest.manifest_seq = Sequence{};
    manifest.commit_seq = Sequence{};
    manifest.snapshot_seq = Sequence{};
    manifest.active_segment = 0;
    manifest.active_offset = 0;
    manifest.authority_revision = state.authority.revision;
    manifest.control_epoch = state.authority.control_epoch;
    manifest.mint_ordinal = state.mint_ordinal;
    manifest.idempotency_capacity = state.idempotency_capacity;
    manifest.head_digest = state.state_digest();
    manifest.snapshot_digest = Sha256Digest{};
    manifest.prev_manifest_digest = Sha256Digest{};
    manifest.head_record_digest = Sha256Digest{};
    manifest.manifest_digest = Sha256::hash(std::span<const std::uint8_t>{});

    auto active = RandomAccessFile::open(segment_path(options.directory, 0), true);
    if (!active) {
      return active.error();
    }
    auto truncated = active.value().truncate(0);
    if (!truncated) {
      return truncated.error();
    }
    auto flushed = active.value().flush();
    if (!flushed) {
      return flushed.error();
    }
    auto directory_flushed = flush_directory(options.directory);
    if (!directory_flushed) {
      return directory_flushed.error();
    }

    const std::vector<std::uint8_t> manifest_bytes = manifest.encode();
    manifest.manifest_digest = Sha256::hash(std::span<const std::uint8_t>(
        manifest_bytes.data(), kManifestDigestOffset));
    const std::vector<std::uint8_t> sealed = manifest.encode();
    auto manifest_file = RandomAccessFile::open(manifest_path(options.directory, 0), true);
    if (!manifest_file) {
      return manifest_file.error();
    }
    auto manifest_written = manifest_file.value().write_at(0, sealed);
    if (!manifest_written) {
      return manifest_written.error();
    }
    auto manifest_flushed = manifest_file.value().flush();
    if (!manifest_flushed) {
      return manifest_flushed.error();
    }
    auto read_back = manifest_file.value().read_at(0, sealed.size());
    if (!read_back || read_back.value() != sealed) {
      return Error(ErrorCode::kIntegrityFailure, "fresh manifest did not read back byte-identically");
    }

    std::vector<std::uint8_t> fence_bytes(kFenceFileSize, 0u);
    FenceRecord fence;
    fence.high_water_seq = Sequence{};
    const std::vector<std::uint8_t> fence_slot = fence.encode();
    std::copy(fence_slot.begin(), fence_slot.end(), fence_bytes.begin());
    auto fence_written = write_file_atomic(fence_path(options.directory), fence_bytes, 0);
    if (!fence_written) {
      return fence_written.error();
    }

    store->manifest_ = manifest;
    store->active_slot_ = 0;
    store->active_file_ = std::move(active.value());
    store->active_offset_ = 0;
    store->segment_count_ = 1u;
    store->head_record_digest_ = Sha256Digest{};
    result.state = std::move(state);
    result.report.commit_seq = Sequence{};
    result.report.snapshot_seq = Sequence{};
    result.report.state_digest = manifest.head_digest;
    result.report.manifest_digest = manifest.manifest_digest;
    result.report.entitlements = 0;
    result.report.opened = true;
    result.store = std::move(store);
    return result;
  }

  store->manifest_ = recovery.value().manifest;
  store->active_slot_ = recovery.value().active_slot;
  store->active_offset_ = static_cast<std::uint64_t>(recovery.value().active_offset);
  store->head_record_digest_ = recovery.value().head_record_digest;
  store->fence_high_water_ = recovery.value().fence_high_water;
  auto active = RandomAccessFile::open(segment_path(options.directory, store->manifest_.active_segment), true);
  if (!active) {
    return active.error();
  }
  store->active_file_ = std::move(active.value());
  store->segment_count_ = 1u;
  result.state = std::move(recovery.value().state);
  result.store = std::move(store);
  return result;
}

Result<DurableOpenResult> DurableStore::open_read_only(const StoreOpenOptions& options) {
  auto valid = validate_options(options);
  if (!valid) {
    return valid.error();
  }
  auto recovery = recover_directory(options.directory, options, false);
  if (!recovery) {
    return recovery.error();
  }
  DurableOpenResult result;
  result.report = recovery.value().report;
  result.report.writer_lock_free = false;
  result.state = std::move(recovery.value().state);
  return result;
}

Result<void> DurableStore::rotate_segment() {
  if (manifest_.active_segment == 0xFFFFFFFFu) {
    return Error(ErrorCode::kCapacityExhausted, "journal segment index space is exhausted");
  }
  const std::uint32_t next_index = manifest_.active_segment + 1u;
  const std::filesystem::path path = segment_path(directory_, next_index);
  (void)remove_file_if_exists(path);
  auto file = RandomAccessFile::open(path, true);
  if (!file) {
    return file.error();
  }
  auto truncated = file.value().truncate(0);
  if (!truncated) {
    return truncated.error();
  }
  auto flushed = file.value().flush();
  if (!flushed) {
    return flushed.error();
  }
  auto directory_flushed = flush_directory(directory_);
  if (!directory_flushed) {
    return directory_flushed.error();
  }
  active_file_ = std::move(file.value());
  manifest_.active_segment = next_index;
  active_offset_ = 0;
  segment_count_ += 1u;
  return {};
}

Result<void> DurableStore::publish_manifest(const StateCore& state_after, Sha256Digest head_record_digest,
                                            std::uint32_t active_segment, std::uint64_t active_offset) {
  Manifest next;
  next.version = kManifestVersion;
  next.kind = 1;
  next.reserved = 0;
  next.reserved2 = 0;
  next.reserved3 = 0;
  auto next_manifest_seq = manifest_.manifest_seq.next();
  if (!next_manifest_seq.has_value()) {
    return Error(ErrorCode::kCapacityExhausted, "manifest publication counter is exhausted");
  }
  next.manifest_seq = next_manifest_seq.value();
  next.commit_seq = state_after.commit_seq;
  next.snapshot_seq = manifest_.snapshot_seq;
  next.snapshot_digest = manifest_.snapshot_digest;
  next.active_segment = active_segment;
  next.active_offset = active_offset;
  next.authority_revision = state_after.authority.revision;
  next.control_epoch = state_after.authority.control_epoch;
  next.mint_ordinal = state_after.mint_ordinal;
  next.idempotency_capacity = state_after.idempotency_capacity;
  next.head_digest = state_after.state_digest();
  next.prev_manifest_digest = manifest_.manifest_digest;
  next.head_record_digest = head_record_digest;

  const int next_slot = 1 - active_slot_;
  const std::vector<std::uint8_t> bytes = next.encode();
  // Manifest::encode() always seals the encoding with its own digest; the
  // in-memory copy must carry exactly the digest that the bytes contain, or the
  // next commit would record a predecessor digest that no reader can verify.
  next.manifest_digest =
      Sha256::hash(std::span<const std::uint8_t>(bytes.data(), kManifestDigestOffset));
  auto file = RandomAccessFile::open(manifest_path(directory_, next_slot), true);
  if (!file) {
    return file.error();
  }
  auto written = file.value().write_at(0, bytes);
  if (!written) {
    return written.error();
  }
  auto flushed = file.value().flush();
  if (!flushed) {
    return flushed.error();
  }
  auto read_back = file.value().read_at(0, bytes.size());
  if (!read_back) {
    return read_back.error();
  }
  if (read_back.value() != bytes) {
    return Error(ErrorCode::kIntegrityFailure, "manifest did not read back byte-identically");
  }
  bytes_written_ += bytes.size();
  manifest_ = next;
  active_slot_ = next_slot;
  return {};
}

Result<void> DurableStore::advance_fence(Sequence seq) {
  if (seq <= fence_high_water_) {
    return {};
  }
  FenceRecord record;
  record.high_water_seq = seq;
  const std::vector<std::uint8_t> bytes = record.encode();
  const int slot = 1 - fence_slot_;
  auto file = RandomAccessFile::open(fence_path(directory_), true);
  if (!file) {
    fence_write_failures_ += 1u;
    return {};
  }
  auto written = file.value().write_at(static_cast<std::uint64_t>(slot) * kFenceSlotSize, bytes);
  if (!written) {
    fence_write_failures_ += 1u;
    return {};
  }
  auto flushed = file.value().flush();
  if (!flushed) {
    fence_write_failures_ += 1u;
    return {};
  }
  auto read_back = file.value().read_at(static_cast<std::uint64_t>(slot) * kFenceSlotSize, bytes.size());
  if (!read_back || read_back.value() != bytes) {
    fence_write_failures_ += 1u;
    return {};
  }
  fence_high_water_ = seq;
  fence_slot_ = slot;
  bytes_written_ += bytes.size();
  return {};
}

Result<void> DurableStore::commit(const JournalEntry& entry, const StateCore& state_after) {
  if (read_only_) {
    return Error(ErrorCode::kReadOnlyStore, "this store was opened read-only");
  }
  if (entry.outcome.code != ErrorCode::kOk) {
    return Error(ErrorCode::kInternalError, "refusing to commit a refused outcome");
  }
  if (entry.outcome.commit_seq != state_after.commit_seq) {
    return Error(ErrorCode::kInternalError, "journal entry and state disagree about the commit sequence");
  }
  if (state_after.commit_seq.value() != manifest_.commit_seq.value() + 1u) {
    return Error(ErrorCode::kInternalError, "commit sequence must advance by exactly one");
  }
  const auto observer = options_.commit_stage_observer;

  const std::vector<std::uint8_t> payload = entry.encode();
  if (payload.size() > options_.max_journal_entry_bytes) {
    return Error(ErrorCode::kLimitExceeded, "journal entry exceeds max_journal_entry_bytes",
                 std::to_string(payload.size()));
  }

  LogRecordHeader header;
  header.kind = entry.kind;
  header.seq = state_after.commit_seq;
  header.prev_record_digest = head_record_digest_;
  const std::vector<std::uint8_t> record = encode_log_record(header, payload);
  const Sha256Digest record_digest =
      Sha256::hash(std::span<const std::uint8_t>(record.data(), record.size() - kLogRecordTrailerSize));

  if (observer) {
    observer(CommitStage::kBeforeAppend);
  }
  if (active_offset_ > 0u &&
      active_offset_ + record.size() > static_cast<std::uint64_t>(options_.max_segment_bytes)) {
    auto rotated = rotate_segment();
    if (!rotated) {
      return rotated.error();
    }
    if (observer) {
      observer(CommitStage::kAfterSegmentRotateBeforeManifest);
    }
  }
  auto written = active_file_.write_at(active_offset_, record);
  if (!written) {
    return written.error();
  }
  if (observer) {
    observer(CommitStage::kAfterAppendBeforeFlush);
  }
  auto flushed = active_file_.flush();
  if (!flushed) {
    return flushed.error();
  }
  auto read_back = active_file_.read_at(active_offset_, record.size());
  if (!read_back) {
    return read_back.error();
  }
  if (read_back.value() != record) {
    return Error(ErrorCode::kIntegrityFailure, "journal record did not read back byte-identically");
  }
  if (observer) {
    observer(CommitStage::kAfterFlushBeforeManifest);
  }

  auto published = publish_manifest(state_after, record_digest, manifest_.active_segment,
                                    active_offset_ + record.size());
  if (!published) {
    return published.error();
  }
  active_offset_ += record.size();
  head_record_digest_ = record_digest;
  bytes_written_ += record.size();
  if (observer) {
    observer(CommitStage::kAfterManifestPublish);
  }
  auto fenced = advance_fence(manifest_.commit_seq);
  if (!fenced) {
    return fenced.error();
  }
  return {};
}

Result<CompactionReport> DurableStore::write_snapshot(const StateCore& state) {
  CompactionReport report;
  if (read_only_) {
    return Error(ErrorCode::kReadOnlyStore, "this store was opened read-only");
  }
  if (state.commit_seq != manifest_.commit_seq) {
    return Error(ErrorCode::kInternalError, "snapshot request does not match the committed sequence");
  }
  const std::vector<std::uint8_t> bytes = encode_snapshot(state);
  if (bytes.size() > kMaxSnapshotBytes) {
    return Error(ErrorCode::kLimitExceeded, "snapshot exceeds the supported size bound");
  }
  const std::filesystem::path path = snapshot_path(directory_, state.commit_seq);
  auto published = write_file_atomic(path, bytes, state.commit_seq.value());
  if (!published) {
    return published.error();
  }
  auto read_back = read_file(path, kMaxSnapshotBytes + 4096u);
  if (!read_back) {
    return read_back.error();
  }
  if (read_back.value() != bytes) {
    return Error(ErrorCode::kIntegrityFailure, "snapshot did not read back byte-identically");
  }
  const Sha256Digest digest = state.state_digest();
  manifest_.snapshot_seq = state.commit_seq;
  manifest_.snapshot_digest = digest;
  auto written = publish_manifest(state, head_record_digest_, manifest_.active_segment, active_offset_);
  if (!written) {
    return written.error();
  }
  bytes_written_ += bytes.size();

  auto entries = list_directory(directory_);
  if (!entries) {
    return entries.error();
  }
  for (const std::filesystem::path& entry : entries.value()) {
    const std::string name = entry.filename().string();
    std::uint32_t index = 0;
    if (parse_segment_name(name, index) && index < manifest_.active_segment) {
      const auto size = file_size_bytes(directory_ / entry.filename());
      if (size.has_value()) {
        report.bytes_reclaimed += size.value();
      }
      (void)remove_file_if_exists(directory_ / entry.filename());
      report.segments_removed += 1u;
      continue;
    }
    Sequence seq{};
    if (parse_snapshot_name(name, seq) && seq != manifest_.snapshot_seq) {
      const auto size = file_size_bytes(directory_ / entry.filename());
      if (size.has_value()) {
        report.bytes_reclaimed += size.value();
      }
      (void)remove_file_if_exists(directory_ / entry.filename());
    }
  }
  report.performed = true;
  report.snapshot_seq = manifest_.snapshot_seq;
  report.message = "snapshot published at commit sequence " + std::to_string(manifest_.snapshot_seq.value());
  return report;
}

}  // namespace entl::detail
