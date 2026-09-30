// Resource Entitlement — the durable entitlement store.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_STORE_HPP
#define RESOURCE_ENTITLEMENT_STORE_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "resource_entitlement/binding.hpp"
#include "resource_entitlement/decision.hpp"
#include "resource_entitlement/entitlement.hpp"
#include "resource_entitlement/error.hpp"
#include "resource_entitlement/events.hpp"
#include "resource_entitlement/request.hpp"
#include "resource_entitlement/token.hpp"

namespace entl {

/// Durable commit stages at which an observer may be invoked. Each value names
/// the state of the durable log at that instant.
enum class CommitStage : std::uint8_t {
  /// Nothing has been written for this commit yet.
  kBeforeAppend = 1,
  /// The record bytes are in the process write buffer but not flushed.
  kAfterAppendBeforeFlush = 2,
  /// The record has been flushed and read back; the manifest has not moved, so
  /// the commit is not yet authoritative.
  kAfterFlushBeforeManifest = 3,
  /// The manifest has been atomically published; the commit is authoritative.
  kAfterManifestPublish = 4,
  /// A new log segment has been created and flushed but the manifest does not
  /// yet name it.
  kAfterSegmentRotateBeforeManifest = 5,
};

[[nodiscard]] const char* commit_stage_name(CommitStage stage) noexcept;

/// Invoked inline at each durable commit stage.
///
/// The observer runs while the store's commit serialization is held, so it must
/// not call back into the store, must not block, and must not throw. Its purpose
/// is durability instrumentation: crash-consistency tooling terminates the
/// process at a chosen stage to prove that recovery never observes a partially
/// applied commit. Production callers leave it unset.
using CommitStageObserver = std::function<void(CommitStage)>;

struct StoreOpenOptions {
  std::filesystem::path directory{};

  /// Create the store directory and its initial durable state if absent.
  bool create_if_missing{false};

  /// Open for inspection without taking the writer lock and without ever
  /// mutating the directory. Mutating calls fail with kReadOnlyStore.
  bool read_only{false};

  /// When true, opening a store that already holds live authority atomically
  /// advances the control epoch and fences that authority. Use this when live
  /// authority must not survive a process restart.
  bool fence_live_authority_on_open{false};

  /// Rotate to a new log segment once the active segment would exceed this.
  std::uint32_t max_segment_bytes{4u * 1024u * 1024u};

  /// Reject any single journal entry larger than this.
  std::uint32_t max_journal_entry_bytes{256u * 1024u};

  /// Bound on the durable lost-response window. Requests older than this many
  /// committed mutations are no longer replayable from the idempotency index.
  std::uint32_t max_idempotency_entries{8192};

  /// When true, a grant whose admission decision digest is the all-zero digest
  /// is refused with kMissingAdmissionEvidence.
  bool require_admission_evidence{true};

  CommitStageObserver commit_stage_observer{};
  EventSink event_sink{};
};

struct StoreStats {
  Sequence commit_seq{};
  Revision authority_revision{};
  Epoch control_epoch{};
  MintOrdinal mint_ordinal{};
  std::size_t entitlement_count{};
  std::size_t live_count{};
  std::size_t terminal_count{};
  std::size_t idempotency_entries{};
  std::size_t log_segment_count{};
  std::uint64_t log_bytes{};
  std::uint64_t bytes_written{};
  std::uint64_t commits{};
  std::uint64_t replays{};
  std::uint64_t fence_write_failures{};
};

struct ListFilter {
  std::optional<TenantId> holder{};
  std::optional<ServiceId> service{};
  std::optional<ResourceScope> scope{};
  std::optional<EntitlementState> state{};
  std::optional<LineageRelation> relation{};
  std::optional<EntitlementId> root{};
  std::optional<EntitlementId> derived_from{};
  bool live_only{false};
  Timestamp now{};
  std::size_t limit{1024};
};

struct LineageNode {
  EntitlementId id{};
  LineageRelation relation{LineageRelation::kRoot};
  std::vector<EntitlementId> sources{};
  TenantId holder{};
  EntitlementState state{EntitlementState::kActive};
  Revision revision{};
  Timestamp created_at{};
  Quantity granted{};
  Quantity remaining{};
  Quantity delegated_out{};
  bool live{false};
  ErrorCode liveness_code{ErrorCode::kOk};
  std::optional<Timestamp> revoked_at{};
  TerminalReason terminal_reason{TerminalReason::kNotApplicable};
};

struct LineageView {
  EntitlementId root{};

  /// Every record in the lineage, ordered by (created_at, id). Ordering is
  /// historical; it never implies that an older record still holds authority.
  std::vector<LineageNode> nodes{};

  /// Every recorded holder change across the lineage, in commit order.
  std::vector<HolderChange> holder_history{};
};

struct OrderRequest {
  Timestamp now{};
  std::optional<TenantId> holder{};
  std::optional<ResourceScope> scope{};
  std::optional<Unit> unit{};
};

struct OrderEntry {
  EntitlementId id{};
  Priority priority{};
  Quantity remaining{};
  Timestamp effective_from{};
  Timestamp expires_at{};
};

struct ExcludedEntry {
  EntitlementId id{};
  ErrorCode code{ErrorCode::kOk};
  std::string explanation{};
};

struct OrderResult {
  std::vector<OrderEntry> entries{};
  std::vector<ExcludedEntry> excluded{};
};

/// A read-only inspection of a store directory. Inspection performs the same
/// integrity and rollback checks as an ordinary open and never takes the writer
/// lock.
struct InspectionReport {
  bool opened{false};
  ErrorCode code{ErrorCode::kOk};
  std::string message{};
  std::uint16_t format_version{0};
  Sequence commit_seq{};
  Sequence snapshot_seq{};
  Sha256Digest state_digest{};
  Sha256Digest manifest_digest{};
  bool manifest_slots_consistent{false};
  bool rollback_detected{false};
  bool writer_lock_free{false};
  bool torn_tail_discarded{false};
  std::size_t segments_scanned{0};
  std::size_t records_replayed{0};
  std::size_t entitlements{0};
  std::size_t orphan_records{0};
  std::uint64_t bytes_scanned{0};
  std::vector<std::string> findings{};
};

struct CompactionReport {
  bool performed{false};
  Sequence snapshot_seq{};
  std::size_t records_compacted{0};
  std::size_t segments_removed{0};
  std::uint64_t bytes_reclaimed{0};
  std::string message{};
};

/// The durable entitlement ledger.
///
/// Concurrency model: a single instance serializes every mutation and
/// publication behind one internal exclusive mutex, and serves reads under a
/// shared mutex. Cross-process exclusion is provided by an operating-system
/// advisory lock on <directory>/entitlement.lock, which the kernel releases
/// when the owning process dies. A second writer fails fast with
/// kWriterLockHeld; it never queues.
class Store {
public:
  [[nodiscard]] static Result<std::unique_ptr<Store>> open(const StoreOpenOptions& options);

  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;

  [[nodiscard]] bool is_read_only() const noexcept;
  [[nodiscard]] const std::filesystem::path& directory() const noexcept;
  [[nodiscard]] AuthoritySnapshot authority() const;
  [[nodiscard]] Sequence commit_seq() const;
  [[nodiscard]] StoreStats stats() const;

  Result<MutationOutcome> update_authority(const AuthorityUpdateRequest& request);
  Result<MutationOutcome> grant(const GrantRequest& request);
  Result<MutationOutcome> suspend(const SuspendRequest& request);
  Result<MutationOutcome> resume(const ResumeRequest& request);
  Result<MutationOutcome> revoke(const RevokeRequest& request);
  Result<MutationOutcome> expire(const ExpireRequest& request);
  Result<MutationOutcome> transfer(const TransferRequest& request);
  Result<MutationOutcome> merge(const MergeRequest& request);
  Result<MutationOutcome> reissue(const ReissueRequest& request);
  Result<MutationOutcome> draw(const DrawRequest& request);
  Result<MutationOutcome> release(const ReleaseRequest& request);

  [[nodiscard]] Result<Entitlement> get(const EntitlementId& id) const;
  [[nodiscard]] Result<std::vector<Entitlement>> list(const ListFilter& filter) const;
  [[nodiscard]] Result<LineageView> lineage(const EntitlementId& id) const;
  [[nodiscard]] Result<OrderResult> order_for_consumption(const OrderRequest& request) const;
  [[nodiscard]] Result<VerificationDecision> verify(const VerifyRequest& request) const;
  [[nodiscard]] Result<VerificationDecision> verify_token(const VerifyTokenRequest& request) const;
  [[nodiscard]] Result<EntitlementToken> issue_token(const EntitlementId& id) const;

  /// Returns the durable outcome of a previously committed request, if it is
  /// still inside the idempotency window.
  [[nodiscard]] Result<MutationOutcome> lookup_outcome(const RequestId& request_id) const;

  Result<CompactionReport> compact();

  /// Inspects a store directory without acquiring the writer lock.
  [[nodiscard]] static Result<InspectionReport> inspect(const std::filesystem::path& directory);

private:
  struct Impl;
  explicit Store(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_STORE_HPP
