// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal durable store format. Not installed.

#ifndef RESOURCE_ENTITLEMENT_SRC_DURABLE_HPP
#define RESOURCE_ENTITLEMENT_SRC_DURABLE_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fileio.hpp"
#include "resource_entitlement/entitlement.hpp"
#include "resource_entitlement/request.hpp"
#include "resource_entitlement/store.hpp"

namespace entl::detail {

// ---------------------------------------------------------------------------
// On-disk layout (store format version 1)
//
//   <dir>/entitlement.lock          zero-length advisory lock target
//   <dir>/manifest.a, manifest.b    fixed 512-byte manifests, alternating slots
//   <dir>/fence.bin                 fixed 128-byte rollback high-water mark
//   <dir>/log-XXXXXXXX.bin          append-only journal segments
//   <dir>/snapshot-XXXXXXXXXXXXXXXX.bin  full state snapshots
//
// Commit order: append record -> flush -> read back and verify -> publish
// manifest into the inactive slot -> flush -> read back and verify -> advance
// fence. The manifest publication is the single atomic commit point.
// ---------------------------------------------------------------------------

inline constexpr std::uint16_t kManifestVersion = 1;
inline constexpr std::uint16_t kFenceVersion = 1;
inline constexpr std::uint16_t kLogRecordVersion = 1;
inline constexpr std::uint16_t kJournalEntryVersion = 1;
inline constexpr std::uint16_t kSnapshotVersion = 1;
inline constexpr std::uint16_t kStateVersion = 1;

inline constexpr std::size_t kManifestFileSize = 512;
inline constexpr std::size_t kManifestFieldsSize = 212;
inline constexpr std::size_t kManifestCrcOffset = 212;
inline constexpr std::size_t kManifestDigestOffset = 216;
inline constexpr std::size_t kManifestUsedSize = 248;
inline constexpr std::size_t kFenceSlotSize = 128;
inline constexpr std::size_t kFenceFileSize = 256;
inline constexpr std::size_t kFenceFieldsSize = 32;
inline constexpr std::size_t kFenceCrcOffset = 32;
inline constexpr std::size_t kFenceDigestOffset = 36;
inline constexpr std::size_t kFenceUsedSize = 68;
inline constexpr std::size_t kLogRecordHeaderSize = 88;
inline constexpr std::size_t kLogRecordTrailerSize = 4;
inline constexpr std::size_t kMaxSnapshotBytes = 1ull << 30;
inline constexpr std::uint32_t kMinSegmentBytes = 4096;
inline constexpr std::uint32_t kMaxSegmentBytes = 1u << 30;
inline constexpr std::uint32_t kMinJournalEntryBytes = 1024;
inline constexpr std::uint32_t kMaxJournalEntryBytes = 64u << 20;
inline constexpr std::uint32_t kMinIdempotencyCapacity = 16;
inline constexpr std::uint32_t kMaxIdempotencyCapacity = 1u << 20;

[[nodiscard]] std::filesystem::path lock_path(const std::filesystem::path& directory);
[[nodiscard]] std::filesystem::path manifest_path(const std::filesystem::path& directory, int slot);
[[nodiscard]] std::filesystem::path fence_path(const std::filesystem::path& directory);
[[nodiscard]] std::filesystem::path segment_path(const std::filesystem::path& directory, std::uint32_t index);
[[nodiscard]] std::filesystem::path snapshot_path(const std::filesystem::path& directory, Sequence seq);

/// Parses "log-00000001.bin" and "snapshot-0000000000000001.bin" names.
[[nodiscard]] bool parse_segment_name(std::string_view name, std::uint32_t& index) noexcept;
[[nodiscard]] bool parse_snapshot_name(std::string_view name, Sequence& seq) noexcept;

/// Validates the segment/journal bounds in an option set.
[[nodiscard]] Result<void> validate_options(const StoreOpenOptions& options);

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

struct Manifest {
  std::uint16_t version{kManifestVersion};
  std::uint16_t kind{1};
  std::uint32_t reserved{0};

  /// Publication counter. Strictly increases with every manifest write,
  /// including a snapshot publication that leaves the commit sequence
  /// unchanged; it is the only reliable ordering key for the two slots.
  Sequence manifest_seq{};
  Sequence commit_seq{};
  Sequence snapshot_seq{};
  std::uint32_t active_segment{0};
  std::uint32_t reserved2{0};
  std::uint64_t active_offset{0};
  Revision authority_revision{};
  Epoch control_epoch{};
  MintOrdinal mint_ordinal{};
  std::uint32_t idempotency_capacity{0};
  std::uint32_t reserved3{0};
  Sha256Digest head_digest{};
  Sha256Digest snapshot_digest{};
  Sha256Digest prev_manifest_digest{};
  Sha256Digest head_record_digest{};
  Sha256Digest manifest_digest{};

  /// Encodes the fixed-size slot. encode() always seals the encoding with the
  /// digest of its own covered prefix, so the manifest_digest member is only
  /// authoritative after decode() or after it has been read back from those
  /// exact bytes.
  [[nodiscard]] std::vector<std::uint8_t> encode() const;
  [[nodiscard]] static Result<Manifest> decode(std::span<const std::uint8_t> bytes);
};

struct FenceRecord {
  std::uint16_t version{kFenceVersion};
  std::uint16_t kind{1};
  std::uint32_t reserved{0};
  Sequence high_water_seq{};
  std::uint64_t reserved2{0};

  /// encode() produces exactly kFenceSlotSize bytes; the fence file holds two
  /// alternating slots of that size.
  [[nodiscard]] std::vector<std::uint8_t> encode() const;
  [[nodiscard]] static Result<FenceRecord> decode(std::span<const std::uint8_t> bytes);
};

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------

enum class JournalKind : std::uint8_t {
  kAuthorityUpdate = 1,
  kGrant = 2,
  kSuspend = 3,
  kResume = 4,
  kRevoke = 5,
  kExpire = 6,
  kMove = 7,
  kSplit = 8,
  kDelegate = 9,
  kMerge = 10,
  kReissue = 11,
  kDraw = 12,
  kRelease = 13,
  kFenceOnOpen = 14,
};

[[nodiscard]] bool is_valid_journal_kind(std::uint8_t kind) noexcept;
[[nodiscard]] const char* journal_kind_name(std::uint8_t kind) noexcept;

struct JournalEntry {
  std::uint8_t kind{0};
  MutationOutcome outcome{};
  std::vector<Entitlement> post_images{};
  AuthoritySnapshot authority_after{};
  bool authority_changed{false};

  [[nodiscard]] std::vector<std::uint8_t> encode() const;
  [[nodiscard]] static Result<JournalEntry> decode(std::span<const std::uint8_t> bytes);
};

struct LogRecordHeader {
  std::uint16_t version{kLogRecordVersion};
  std::uint8_t kind{0};
  std::uint8_t reserved0{0};
  std::uint32_t flags{0};
  std::uint32_t payload_length{0};
  Sequence seq{};
  Sha256Digest prev_record_digest{};
  Sha256Digest payload_digest{};
  Sha256Digest record_digest{};
};

[[nodiscard]] std::vector<std::uint8_t> encode_log_record(const LogRecordHeader& header,
                                                          std::span<const std::uint8_t> payload);

struct DecodedLogRecord {
  LogRecordHeader header{};
  std::span<const std::uint8_t> payload{};
  std::size_t total_length{0};
};

/// Decodes one record from \p bytes. Returns kTruncatedEncoding when the buffer
/// holds a partial record, which is legitimate only at the tail of the active
/// segment. Every other malformation is reported as kCorruptLog.
[[nodiscard]] Result<DecodedLogRecord> decode_log_record(std::span<const std::uint8_t> bytes);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct IdempotencyEntry {
  RequestId request_id{};
  MutationOutcome outcome{};
};

/// The complete durable state. Its canonical encoding is both the snapshot
/// payload and the input of the manifest head digest.
struct StateCore {
  AuthoritySnapshot authority{};
  std::vector<Entitlement> entitlements{};
  std::vector<IdempotencyEntry> idempotency{};
  MintOrdinal mint_ordinal{};
  std::uint32_t idempotency_capacity{8192};
  Sequence commit_seq{};

  [[nodiscard]] std::vector<std::uint8_t> encode() const;
  [[nodiscard]] static Result<StateCore> decode(std::span<const std::uint8_t> bytes);
  [[nodiscard]] Sha256Digest state_digest() const;

  [[nodiscard]] const Entitlement* find(const EntitlementId& id) const;
  [[nodiscard]] Entitlement* find(const EntitlementId& id);
  void insert_or_replace(const Entitlement& record);
  void sort_entitlements();
};

/// Inserts a committed outcome into the bounded lost-response window, evicting
/// the oldest entries first. Used identically by the live commit path and by
/// journal replay so that a recovered state digest equals the committed one.
void record_idempotency(StateCore& state, const MutationOutcome& outcome);

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

struct DurableOpenResult {
  std::unique_ptr<class DurableStore> store;
  StateCore state;
  InspectionReport report;
};

/// Owns the writer lock, the active journal segment, and the authoritative
/// manifest for one store directory.
class DurableStore {
public:
  DurableStore();
  ~DurableStore();
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;
  DurableStore(DurableStore&&) = delete;
  DurableStore& operator=(DurableStore&&) = delete;

  /// Opens for writing: takes the writer lock, recovers state, truncates any
  /// uncommitted journal tail, and removes orphan segments.
  [[nodiscard]] static Result<DurableOpenResult> open(const StoreOpenOptions& options);

  /// Opens read-only: no writer lock, no mutation, same integrity and rollback
  /// checks. The returned store is null.
  [[nodiscard]] static Result<DurableOpenResult> open_read_only(const StoreOpenOptions& options);

  /// Appends \p entry, publishes it durably, and advances the fence.
  [[nodiscard]] Result<void> commit(const JournalEntry& entry, const StateCore& state_after);

  /// Writes and publishes a snapshot of \p state, then reclaims superseded
  /// segments and snapshots.
  [[nodiscard]] Result<CompactionReport> write_snapshot(const StateCore& state);

  [[nodiscard]] const Manifest& manifest() const noexcept { return manifest_; }
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
  [[nodiscard]] std::uint64_t bytes_written() const noexcept { return bytes_written_; }
  [[nodiscard]] std::size_t segment_count() const noexcept { return segment_count_; }
  [[nodiscard]] std::uint64_t fence_write_failures() const noexcept { return fence_write_failures_; }

private:
  [[nodiscard]] Result<void> publish_manifest(const StateCore& state_after, Sha256Digest head_record_digest,
                                              std::uint32_t active_segment, std::uint64_t active_offset);
  [[nodiscard]] Result<void> advance_fence(Sequence seq);
  [[nodiscard]] Result<void> rotate_segment();

  std::filesystem::path directory_{};
  StoreOpenOptions options_{};
  WriterLock lock_{};
  RandomAccessFile active_file_{};
  Manifest manifest_{};
  int active_slot_{0};
  std::uint64_t active_offset_{0};
  Sha256Digest head_record_digest_{};
  std::size_t segment_count_{0};
  std::uint64_t bytes_written_{0};
  std::uint64_t fence_write_failures_{0};
  Sequence fence_high_water_{};
  int fence_slot_{0};
  bool read_only_{false};
};

}  // namespace entl::detail

#endif  // RESOURCE_ENTITLEMENT_SRC_DURABLE_HPP
