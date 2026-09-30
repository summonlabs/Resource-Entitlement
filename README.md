# Resource Entitlement

Generation-bound facility entitlements with explicit scope, quantity, priority, expiry,
revocation, transfer, and stale-authority fencing, backed by a crash-consistent
single-writer ledger.

* **DCCP position:** repository 47 of 72, Tranche 6 — Facility Policy, Tenancy, and Entitlement.
* **Language / build:** C++20, CMake, no third-party runtime dependency.
* **Public API namespace:** `entl`, headers under `resource_entitlement/`.
* **Installed package:** `ResourceEntitlement`, imported target
  `ResourceEntitlement::resource_entitlement`.
* **Command line tool:** `entl`.

## Core question

> Is this entitlement live authority for this tenant, this service, this scope, and this
> quantity, *right now*, under the authority generations that actually justify it?

Everything in this repository exists to answer that question deterministically and to keep
the answer durable, attributable, and fenced.

## What this runtime is

Resource Entitlement is the facility-side ledger of *authority to consume*. It records that a
tenant has been granted the right to consume a bounded quantity of a bounded resource scope,
under an explicit service class, priority, and validity window, justified by named evidence
from authorities that it does not own.

An entitlement is **authority, not allocation**. Recording a grant does not create capacity,
does not reserve hardware, does not place anything, and does not prove that anything was
consumed. It records what a tenant is entitled to consume and how much of that entitlement
has been drawn down.

## Owned boundary

This repository owns exactly:

* **Entitlement identity and lifecycle.** Minting, revisioning, activation, suspension,
  resumption, revocation, expiry, supersession, and terminal accounting.
* **Authority binding.** The exact tenant, service, service class, facility, resource type,
  resource scope, unit, quantity, priority, validity window, and provenance that a grant is
  bound to, together with the generation, revision, and epoch fields and the evidence digests
  that justify it.
* **Fencing.** Deterministic detection that a grant's justification is stale, superseded, or
  fenced by a new control incarnation, and refusal of any use of stale authority.
* **Transfer semantics.** Move, split, delegate, merge, and reissue, with explicit rules that
  make authority duplication impossible.
* **Durable accounting of entitlement state.** The authoritative record of granted, remaining,
  delegated, and drawn quantity, and of every lifecycle transition, surviving crash, restart,
  and compaction.
* **Deterministic verification.** A single decision function with a fixed refusal precedence
  and full secondary evidence.
* **Canonical records and tokens.** Exact, bounded, digestible encodings of a grant for
  transport and inspection.

## Explicit non-ownership

This repository does **not** own, compute, simulate, or approximate any of the following. It
consumes results from the repositories that do.

| Concern | Owner |
| --- | --- |
| Capacity measurement or creation | Facility Capacity, Capacity Fabric |
| Admission decision logic | Facility Admission Control |
| Placement execution | Facility Placement Planner, Facility Placement Policy |
| ASI scheduling and quota enforcement | Agent Scheduler, Inference Scheduler |
| DFI path and bandwidth enforcement | Bandwidth Broker, Path Authority |
| Tenant identity | Tenant Registry |
| Service-class definition | Service-Class Registry |
| Facility policy definition | Facility Policy Engine |
| Envelope definition | Resource Envelope |
| Control-plane epoch ownership | Control-Plane Epoch |
| Billing and charging | Billing systems outside DCCP |

A grant carries an **admission decision digest** and binds to the current authoritative
generations; it does not evaluate admission. When the operator has not yet deployed an
admitting authority, the store can be created with `require_admission_evidence = false`, in
which case a grant may carry the all-zero digest — an explicitly modelled "no evidence
supplied" value, never a silent zero.

## Principal invariants

1. **Observation is not authority.** Reading a record, or holding a token that encodes one,
   confers nothing. Every decision re-reads the durable ledger.
2. **Acknowledgement is not effect.** A mutation is accepted only after its manifest
   publication is flushed and read back byte-identically.
3. **Requested state is not observed state.** Verification answers describe what the ledger
   held at the decision's commit sequence, not what a caller asked for.
4. **Missing, unknown, stale, or undetermined data never becomes zero, false, healthy, or
   permitted.** The zero digest, the zero timestamp, and the zero counter are explicit
   "not supplied" markers that every boundary rejects where evidence is required.
5. **Recovered state is not automatically fresh authority.** Authority is durable and portable
   across restarts of the same store directory, because the control epoch is part of the
   durable state; when a deployment requires that live authority must not survive a process
   restart, opening with `fence_live_authority_on_open` atomically advances the control epoch
   and fences everything granted before the restart.
6. **Stale authority is fenced, not inherited.** A change to a fenced generation, revision,
   digest, or the control epoch makes every bound grant refuse with a specific diagnostic until
   it is explicitly re-established by `reissue`.
7. **Idempotent replay is resolved before ordinary staleness rejection.** A retried request
   whose identity was already committed returns the recorded outcome verbatim, even though its
   expected revision is now stale.
8. **Authority is never duplicated.** Move transfers the same record; split and delegate mint a
   new record and deduct exactly the transferred quantity from the source's usable remainder
   in the same commit; merge sums two remainders and supersedes both sources.
9. **Priority is advisory.** Priority is recorded and used for deterministic ordering only. It
   never preempts, suspends, revokes, or shortens another entitlement.
10. **Terminal states never authorize.** Revoked, expired, and superseded records keep their
    historical accounting and confer nothing.
11. **The store never persists a record its own reader would reject.** Every post-image is
    re-encoded, re-decoded, and re-encoded again before a commit, and must round trip byte for
    byte.
12. **Accounting is conservative.** `remaining <= granted` always holds; release can never
    return more authority than was granted; every counter advance is checked for overflow.

## Authority, generations, and fencing

### The authoritative snapshot

The store holds exactly one *authoritative snapshot*: the generations, revisions, and evidence
digests that new grants bind to. It is **reported to** this repository, never derived by it:

| Field | Reported by |
| --- | --- |
| `facility_capacity_generation` | Facility Capacity |
| `facility_policy_revision` | Facility Policy Engine |
| `resource_envelope_revision` | Resource Envelope |
| `service_class_revision` | Service-Class Registry |
| `control_epoch` | this repository's control plane |
| `policy_context_digest` | Facility Policy Engine |
| `envelope_binding_digest` | Resource Envelope |

A snapshot is published with `authority` (CLI) or `Store::update_authority` (library), which
is a normal commit: it advances the snapshot revision, changes exactly the fields the caller
supplied, optionally advances the control epoch, and is fenced by the same durable machinery as
any other mutation.

### What a grant binds to

Every entitlement carries an `AuthorityBinding`:

* the snapshot revision it was created against;
* the capacity generation, policy revision, envelope revision, and service-class revision;
* the control epoch in force at grant time;
* the policy-context and envelope digests from the snapshot;
* the per-grant **admission decision digest** supplied by the caller.

The canonical digest of that binding is stable, is stored with the record, and is reported by
every verification decision.

### Fence masks

A grant carries a `FenceMask` selecting which binding fields are *fenced*. The default
(`standard`) fences the capacity generation, the policy revision, the envelope revision, the
service-class revision, and the control epoch. `all` additionally fences the policy-context
and envelope digests. `epoch` fences only the control epoch.

The control-epoch bit is **mandatory**: a mask without it is rejected with
`invalid_fence_mask`, because an entitlement whose authority cannot be fenced by a new control
incarnation would be unsafe.

### Which changes invalidate authority

| Change | Effect on existing grants |
| --- | --- |
| Capacity generation advances | Every grant whose mask fences capacity refuses with `stale_binding` |
| Policy, envelope, or service-class revision advances | Same, for the corresponding bit |
| Policy-context or envelope digest changes | Same, when the corresponding bit is fenced |
| Control epoch advances | Every grant bound to an older epoch refuses with `stale_epoch` |
| Admission evidence changes | No effect: evidence is recorded provenance, not a fence |
| `reissue` with `rebind_to_current_authority` | The successor is bound to the current snapshot and becomes live; the source is superseded |

A new control incarnation is published atomically with the state that makes it authoritative:
the epoch and the state digest travel in the same manifest publication.

### The bootstrap snapshot

A freshly created store publishes a bootstrap snapshot with revision 1 and control epoch 1 and
no published generations (generation 0, revision 0, zero digests). Generation 0 is a real
value meaning "the initial generation of that authority"; it is not a silent zero. A grant made
against the bootstrap snapshot is fenced as soon as real generations are published, so the
intended order of operations is: create the store, publish authority, then grant.

## Lifecycle and state model

```
              grant
                |
                v
   +--------> active <---------+
   |            |  \           |
   |        suspend  \ draw / release (accounting only)
   |            v    v        |
   |        suspended ------  (remaining reaches zero: still active, no usable authority)
   |            |
   |          resume
   |            |
   +------------+
                |
     revoke / expire / supersede (reissue, merge)
                v
      revoked | expired | superseded      (terminal, no authority)
```

* **active** — the record may confer authority when its binding is fresh, the decision instant
  is inside `[effective_from, expires_at)`, and `remaining > 0`.
* **suspended** — confers nothing; `resume` restores it if the window has not passed.
* **revoked** — terminal. Revocation may cascade to *delegated* descendants (which derived
  their authority from the revoked record); *split* descendants are independent and are not
  cascaded. Cascading is explicit and can be disabled per request.
* **expired** — terminal, recorded by an explicit sweep. Expiry does **not** require the sweep:
  verification derives expiry from the decision instant, so a record past `expires_at`
  refuses with `expired` even if nothing has swept it.
* **superseded** — terminal, produced by `reissue` or as a merge source. The successor record
  carries the authority; the source never does again.

The validity window is half-open: `[effective_from, expires_at)`. At exactly `expires_at` the
entitlement is already expired.

### Transfers

| Mode | Identity | Quantity | Lineage | Revocation coupling |
| --- | --- | --- | --- | --- |
| `move` | unchanged | whole remaining quantity | `holder_history` gains an entry naming previous and new holder | none |
| `split` | new record | new record receives exactly the split quantity; source's `granted` and `remaining` both fall by it | `split_from` | none |
| `delegate` | new record | new record receives the quantity; source's `remaining` falls and `delegated_out` rises by it | `delegated_from` | revoking the source cascades by default |
| `merge` | new record | sum of both remainders, with checked arithmetic | `merged_from` with two sources | both sources superseded |
| `reissue` | new record | the source's remaining quantity | `reissued_from` | source superseded |

Delegated and split records may be granted a priority that is equal to or **less important**
than the source's, never more important. They inherit the source's validity window exactly, so a
derivative can never outlive its source. Merge requires the same holder, service, service class,
scope, unit, priority, fence mask, and authority generations; the merged window is the
*intersection* of the two windows, so a merge never extends authority.

## Verification semantics

`Store::verify` and `Store::verify_token` return a `VerificationDecision` — a value, never an
exception. A decision always reports:

* `authorized`;
* the primary `ErrorCode` and a human-readable explanation;
* the observed commit sequence and the control epoch the decision was made against;
* the record revision, remaining quantity, and binding digest;
* `stale`, set when any fenced binding field disagreed;
* `secondary_faults`: every other fault observed, in precedence order.

### Deterministic refusal precedence

When several faults coexist, the primary is the first of:

1. `unknown_entitlement`
2. `stale_epoch`
3. `stale_binding`
4. `tenant_mismatch`, `service_mismatch`, `service_class_mismatch`, `scope_mismatch`,
   `unit_mismatch` (expected-context mismatches, in that order)
5. `revoked`
6. `superseded`
7. `suspended`
8. `not_yet_effective`
9. `expired`
10. `quantity_exceeded`

Revocation therefore outranks expiry, and control-epoch fencing outranks everything except an
unknown identity. The suppressed faults are never discarded; they appear in
`secondary_faults`, so a refusal is always attributable.

`verify` requires an explicit decision instant. The zero timestamp means "not supplied" and is
rejected with `invalid_timestamp` rather than being treated as 1970-01-01.

### Tokens

`entl token` and `Store::issue_token` produce a canonical, self-describing encoding of one
record. A token is **not** a bearer credential: it is unsigned, carries no secret, and confers
no authority. `verify-token` re-reads the ledger and additionally requires that the ledger
still holds the exact revision the token describes; a token whose revision has moved on refuses
with `stale_binding`.

## Persistence and recovery

### On-disk layout (store format version 1)

```
<directory>/
  entitlement.lock          advisory single-writer lock target
  manifest.a  manifest.b    two alternating 512-byte manifest slots
  fence.bin                 two alternating 128-byte rollback high-water slots
  log-XXXXXXXX.bin          append-only journal segments
  snapshot-XXXXXXXXXXXXXXXX.bin   full-state snapshots
```

Every fixed-size structure carries a magic value, a format version, a kind, explicit reserved
fields that must be zero, a CRC-32C over its covered prefix, and a SHA-256 seal over that
prefix plus the checksum. Variable-length payloads add a declared length and a payload digest.
All lengths are bounded and validated before allocation; all enums are validated on the way in;
all text is validated as strict UTF-8 without control characters.

### The atomic commit point

A mutation is committed in a fixed order:

1. **stage** — encode the journal entry, validate that every post-image round trips, append the
   record bytes to the active segment;
2. **flush** — flush the segment to stable storage;
3. **read back and verify** — read the record back and require byte-identical content;
4. **publish** — write the new manifest into the *inactive* slot, flush it, and read it back;
5. **advance the fence** — record the new commit sequence in the rollback high-water slot.

**The manifest publication in step 4 is the single atomic commit point.** A record that has been
appended and flushed but whose manifest has not been published is not authoritative and is
discarded on the next open. A crash before step 4 therefore loses nothing that was ever
acknowledged; a crash after step 4 keeps everything, because the manifest names exactly the
committed byte range.

### Manifest slots and publication counter

The two manifest slots alternate. Each publication carries a strictly increasing *publication
counter* (independent of the commit sequence, because a snapshot publication leaves the commit
sequence unchanged) and the digest of its predecessor. Recovery reads both slots, requires both
to be individually verifiable, chooses the one with the higher publication counter, and verifies
that the chosen slot's recorded predecessor digest matches the other slot when the other slot is
its immediate predecessor. Damaged slots are never silently repaired.

### Recovery

Recovery chooses exactly one authoritative generation or fails closed. It never guesses, never
merges, and never starts empty when durable state exists but cannot be verified:

1. Read and verify both manifest slots; pick the highest valid publication counter.
2. Verify the predecessor chain against the other slot.
3. Read the rollback fence. If the fence records a **higher** commit sequence than the manifest,
   the directory has been rolled back to an older copy and recovery fails with
   `rollback_detected`.
4. Load the snapshot named by the manifest and verify its state digest, or fall back to full
   journal replay when it is missing and the journal still covers the whole history.
5. Replay journal records with a contiguous sequence, verifying each record's checksum, payload
   digest, declared length, kind, and predecessor chain. Records the manifest has not published
   are ignored.
6. Require the last replayed record's digest to equal the manifest's head record digest, and the
   recomputed digest of the entire recovered state to equal the manifest's head state digest.
   Any mismatch is `state_digest_mismatch`, never a silent repair.

A writer open additionally truncates an uncommitted journal tail, removes orphan segments left
by an interrupted rotation, and advances the fence. A read-only open performs exactly the same
integrity and rollback checks and mutates nothing.

### Compaction and snapshots

`compact` writes a snapshot of the entire state to a temporary file, flushes it, reads it back
byte-identically, publishes it atomically, then advances the manifest to name it and only then
reclaims superseded segments and older snapshots. A snapshot can never describe a state the
normal reader would refuse, because the reader verifies the snapshot's state digest against the
manifest before using it.

### Single-writer exclusion

`entitlement.lock` is held under an operating-system advisory exclusive lock
(`LockFileEx` on Windows, `flock` on POSIX) for the lifetime of a writer store. The lock is
taken without waiting; a second writer fails immediately with `writer_lock_held` rather than
queueing. The kernel releases the lock when the owning process dies, including abrupt death and
process kill, so no stale lock can outlive its owner. Read-only opens take no writer lock.

## Concurrency model

* **One lock.** A single `std::shared_mutex` guards the in-memory state. Reads take a shared
  lock; every mutation and compaction takes an exclusive lock. There is no other lock in the
  library, and there are no worker threads, futures, or asynchronous completions.
* **No lock upgrades.** No path acquires a shared lock and then attempts to acquire the
  exclusive lock. Mutating entry points take the exclusive lock directly.
* **No reentrancy.** Internal `apply_*` helpers never lock; they are only reachable from a
  mutating entry point that already holds the exclusive lock. `std::shared_mutex` is not
  recursive, and nothing relies on it being recursive.
* **Callbacks are never invoked under the lock.** Events produced by a mutation are queued into
  an internal outbox while the lock is held and delivered by the caller of `mutate` after the
  lock is released. `compact` releases the lock explicitly before emitting.
* **The one documented exception** is the commit-stage observer
  (`StoreOpenOptions::commit_stage_observer`). It is invoked inline at the exact durable stage —
  which is only meaningful while the commit is serialized — and is documented as
  non-reentrant, non-blocking, and non-throwing. The production CLI installs none; it exists for
  crash-consistency instrumentation.
* **Blocking I/O under the lock is deliberate.** A durable commit performs its flush, read-back,
  manifest publication, and fence advance while holding the exclusive lock. Publishing the
  in-memory state before or after the durable publication would break the "acknowledgement is
  not effect" invariant, so serialization is a correctness requirement rather than an
  oversight. Verification is unaffected: it is an in-memory read.
* **No lock inversion is possible.** The writer lock file handle is acquired before any mutex
  exists (at open) and released after all mutexes are gone (at destruction); it is never nested
  with the in-memory mutex.
* **Cross-process behaviour** is exclusion by the writer lock plus, for readers, the same
  integrity and rollback rules that an ordinary open applies. Recovery takes the writer lock
  first, so two processes can never recover the same directory concurrently.
* **Stale asynchronous completion is impossible** because there is no asynchronous completion:
  every operation returns the outcome of the commit it performed.

## Error and refusal semantics

Every fallible entry point returns `entl::Result<T>` holding either a value or an `Error`
with a stable `ErrorCode`, a human-readable message, and optional structured detail. Codes are
grouped and never renumbered, because they are persisted inside idempotency records:

| Range | Meaning | CLI exit code |
| --- | --- | --- |
| 0 | success | 0 |
| 1–19 | input validation | 2 |
| 20–29 | canonical codec | 2 |
| 30–69 | domain refusal by the entitlement semantics | 1 |
| 70–99 | durable storage or operating-system failure | 3 |
| 100+ | internal invariant violation | 1 |

A refusal is a *value*: the caller learns which authority, generation, or constraint refused,
and every suppressed fault is retained. A verification decision is never an error; only a
malformed request or an unreadable ledger is.

### Idempotency and the lost-response window

Every mutating request carries a caller-supplied 128-bit request identity. The durable outcome
of each accepted request is recorded, so a retry whose response was lost returns the original
outcome — same identity, same revision, same commit sequence — without re-executing. A retry
that reuses an identity with a *different* payload is refused with `request_id_conflict`.

The window is bounded (`max_idempotency_entries`, fixed at store creation and persisted). Once
an identity has been evicted, a retry is evaluated as a fresh decision. Double spending is still
prevented, because every mutation also carries an expected revision, and an evicted retry will
fail that precondition. Grants are the one additive operation, so the window is the
lost-response horizon for authority creation; size it accordingly.

## CLI usage

Every command prints exactly one JSON object on standard output. Exit codes are `0` success,
`1` refusal, `2` usage or input error, `3` durable storage failure.

```
entl version
entl selftest
entl init --dir <path> [--idempotency-capacity N]
entl authority --dir <path> --publisher <id> [--capacity-generation N] [--policy-revision N]
                [--envelope-revision N] [--service-class-revision N]
                [--policy-context-digest HEX] [--envelope-binding-digest HEX] [--advance-epoch]
                [--now <instant>] [--note TEXT]
entl grant --dir <path> --request <file|-> [--now <instant>] [--request-id HEX32]
entl show --dir <path> --id <hex>
entl list --dir <path> [--tenant ID] [--service ID] [--state NAME] [--relation NAME]
           [--root HEX] [--derived-from HEX] [--facility ID --resource-type ID --scope ID]
           [--live --now <instant>] [--limit N]
entl verify --dir <path> --id <hex> [--now <instant>] [--quantity N --unit U]
            [--expect-tenant ID --expect-service ID --expect-service-class ID
             --expect-facility ID --expect-resource-type ID --expect-scope ID --expect-unit U]
entl token --dir <path> --id <hex>
entl verify-token --dir <path> --token <hex|file:PATH> [--now <instant>]
entl suspend|resume|expire --dir <path> --id <hex> --revision N --actor ID [--now <instant>]
entl revoke --dir <path> --id <hex> --revision N --actor ID [--reason operator|policy]
            [--cascade true|false] [--now <instant>]
entl draw|release --dir <path> --id <hex> --revision N --quantity N --unit U --actor ID
                   [--now <instant>]
entl transfer --dir <path> --id <hex> --revision N --mode move|split|delegate --target ID
               [--quantity N --unit U] [--priority-class C] [--priority-rank N] --actor ID
               [--now <instant>]
entl merge --dir <path> --first HEX --first-revision N --second HEX --second-revision N
           --actor ID [--now <instant>]
entl reissue --dir <path> --id <hex> --revision N --actor ID [--keep-binding] [--now <instant>]
entl lineage --dir <path> --id <hex>
entl order --dir <path> [--tenant ID] [--facility ID --resource-type ID --scope ID] [--unit U]
            --now <instant>
entl compact --dir <path>
entl inspect --dir <path>
entl stats --dir <path>
entl bench [--dir <path>] [--grants N] [--verifies N] [--draws N] [--preload N] [--keep]
```

**Instants** are RFC 3339 UTC (`2026-02-01T00:00:00Z`, optional fractional seconds) or an
integer nanosecond count. `--now` is the authoritative decision instant; when omitted, the
system clock is read once at the start of the command. Decisions are evaluated at maximally
nondecreasing instants per record: a mutation whose instant precedes the record's creation
instant is refused with `invalid_timestamp` rather than persisted.

**Grant requests** are strict `key=value` documents. Unknown keys, duplicate keys, and missing
required keys are refused; the grammar is line-oriented, comments start with `#`, and the whole
document is bounded.

```
# grant.txt
tenant=tenant-a
service=inference
service-class=gold
facility=dc-1
resource-type=accelerator
scope=pool-a
unit=count
quantity=100
priority-class=standard
priority-rank=0
effective-from=2026-01-01T00:00:00Z
expires-at=2027-01-01T00:00:00Z
admission-digest=3f786850e387550fdab836ed7e6dc881de23001b46f3c96a0e3d8a3a6a3b1c6f
actor=operator-1
note=quarterly allocation
```

When `--request-id` is omitted, the request identity is derived deterministically from the
request document, so an identical document is a replay of the earlier request rather than a
second grant.

### End-to-end example

```console
$ entl init --dir ./ledger
{"ok":true,"directory":".../ledger","authority_revision":1,"control_epoch":1,"commit_seq":0,...}

$ entl authority --dir ./ledger --publisher facility-capacity --capacity-generation 12       --policy-revision 4 --envelope-revision 7 --service-class-revision 3 --now 2026-02-01T00:00:00Z
{"ok":true,"replayed":false,"commit_seq":1,"authority_revision":2,"control_epoch":1,...}

$ entl grant --dir ./ledger --request grant.txt --now 2026-02-01T00:00:00Z
{"ok":true,"replayed":false,"id":"f2a726f6ea16fc8a3e1bcb540c39271e","revision":1,"commit_seq":2,...}

$ entl verify --dir ./ledger --id f2a726f6ea16fc8a3e1bcb540c39271e       --quantity 40 --unit count --now 2026-02-02T00:00:00Z
{"ok":true,"authorized":true,"code":"ok","remaining":{"unit":"count","units":100},...}

$ entl draw --dir ./ledger --id f2a726f6ea16fc8a3e1bcb540c39271e --revision 1       --quantity 40 --unit count --actor workload-controller --now 2026-02-02T00:00:01Z
{"ok":true,"id":"f2a726f6ea16fc8a3e1bcb540c39271e","revision":2,"commit_seq":3,
 "remaining":{"unit":"count","units":60}}

$ entl authority --dir ./ledger --publisher facility-capacity --capacity-generation 13
{"ok":true,...}

$ entl verify --dir ./ledger --id f2a726f6ea16fc8a3e1bcb540c39271e --now 2026-02-02T00:00:02Z
{"ok":true,"authorized":false,"code":"stale_binding","stale":true,...}

$ entl reissue --dir ./ledger --id f2a726f6ea16fc8a3e1bcb540c39271e --revision 2       --actor operator-1 --now 2026-02-02T00:00:03Z
{"ok":true,"id":"...","revision":1,"commit_seq":5,...}
```

## Library integration

```cpp
#include <resource_entitlement/store.hpp>
#include <resource_entitlement/token.hpp>

entl::StoreOpenOptions options;
options.directory = "/var/lib/resource-entitlement";
options.create_if_missing = true;
auto opened = entl::Store::open(options);
if (!opened) {
  // opened.error().code(), .message(), .detail()
}

entl::Store& store = *opened.value();
auto outcome = store.grant(grant_request);
auto decision = store.verify(verify_request);
if (!decision->authorized) {
  // decision->primary, decision->secondary_faults, decision->explanation
}
```

Guidelines:

* One `Store` instance per process per directory; the writer lock makes a second writer fail
  fast with `writer_lock_held`.
* A `Store` is safe to share between threads: reads are concurrent, mutations are serialized.
* Open read-only for inspection and reporting; it neither takes the writer lock nor mutates.
* Treat `VerificationDecision` as the only source of truth about authority; never cache it
  beyond the commit sequence it reports.
* `EventSink` callbacks are delivered after the commit and after the internal lock is released,
  and must not call back into the same store.

## Build, install, and consume

```console
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /opt/resource-entitlement
```

| Option | Default | Meaning |
| --- | --- | --- |
| `RESOURCE_ENTITLEMENT_BUILD_CLI` | `ON` | build the `entl` tool |
| `RESOURCE_ENTITLEMENT_BUILD_TESTS` | `ON` when top-level | build and register the test suite |
| `RESOURCE_ENTITLEMENT_WARNINGS_AS_ERRORS` | `ON` | treat first-party warnings as errors |
| `RESOURCE_ENTITLEMENT_ENABLE_ASAN` | `OFF` | build with the compiler's AddressSanitizer |

First-party code is compiled with `/W4 /permissive- /WX /utf-8 /Zc:__cplusplus /Zc:preprocessor`
on MSVC and with `-Wall -Wextra -Wpedantic -Werror` plus conversion, shadowing, old-style-cast,
and null-pointer-arithmetic diagnostics elsewhere.

Consuming the installed package:

```cmake
find_package(ResourceEntitlement CONFIG REQUIRED)
target_link_libraries(my_tool PRIVATE ResourceEntitlement::resource_entitlement)
```

`tests/downstream/` is a complete out-of-tree consumer that configures, builds, and runs
against an installed prefix only. It exercises publishing authority, granting, verifying,
drawing with idempotent replay, token round trips, suspend/resume, split, lineage, ordering,
capacity fencing, compaction, and durable reopen.

## Validation performed

All results below were produced on the build host described under *Unsupported and unvalidated
behaviour*. Nothing here is projected or estimated.

### Builds

| Configuration | Compiler | Result |
| --- | --- | --- |
| Release, `/W4 /WX /permissive-` | MSVC 19.44.35222.0 (VS 2022 Build Tools 17.14.25) | clean, zero warnings |
| Debug, `/W4 /WX /permissive-` | MSVC 19.44.35222.0 | clean, zero warnings |
| Debug + `/fsanitize=address` | MSVC 19.44.35222.0 | clean, zero warnings |

### Tests

`ctest` reports **14/14 suites passed** in Release, **14/14** in Debug, and **14/14** under
AddressSanitizer, with no timeouts and no skipped cases. The suites are:

| Suite | Obligation |
| --- | --- |
| `test_digest` | SHA-256 and CRC-32C known-answer vectors, block and padding boundaries, streaming vs one-shot equality, hex round trips |
| `test_text_time` | identifier grammar, reserved device names, strict UTF-8 rejection of overlong, surrogate, and out-of-range sequences, text bounds, RFC 3339 round trips, malformed instants, checked time arithmetic |
| `test_canonical` | codec round trips, byte-exact little-endian layout, truncation at every length, trailing bytes, hostile boolean and presence bytes, absurd declared lengths, text validation |
| `test_model` | quantity arithmetic and saturation, priority total order, fence-mask validity, snapshot and binding digest sensitivity, fence evaluation order, liveness precedence, record codec invariants, token tamper detection at every byte and every truncation |
| `test_store_lifecycle` | bootstrap, authority publication and revision preconditions, grant provenance, grant refusal paths, idempotent replay, request-identity conflict, revision preconditions, suspend/resume, revocation, expiry boundary and sweep, draw/release accounting, expected-context mismatches, event delivery, read-only refusal |
| `test_store_transfer` | move and holder history, split conservation, delegation deducting usable authority, transfer refusals, lost-response retry after a transfer, cascading revocation of delegated descendants only, merge arithmetic and supersession, merge incompatibility, reissue and supersession, retried draw |
| `test_store_fencing` | capacity generation invalidating older grants, fence-mask scoping, mandatory control-epoch fencing, atomic epoch advance, reissue rebinding, restart portability, fence-on-open, repeated fence-on-open |
| `test_store_query` | list filters, lineage ordering and liveness, deterministic advisory ordering, token staleness after a revision advance, statistics, bounded idempotency window with oldest-first eviction, compaction and re-recovery |
| `test_durable` | byte-exact restart, segment rotation and reopen, uncommitted tail truncation, writer-lock exclusion, read-only integrity, rollback detection from a restored older manifest, missing fence degradation, empty-directory handling |
| `test_corruption` | single-byte flips across a real manifest, truncation sweeps, non-zero reserved tails, log payload corruption, log truncation at every length, reordered records, impossible declared lengths, forged head digests with repaired checksums, corrupted snapshots |
| `test_process` | a real second process holding the writer lock, kernel lock release after abrupt death, crash at every durable commit stage with recover-and-continue, repeated crash/restart cycles |
| `test_property` | five seeded randomized state machines (300–400 actions each) checking invariants after every action and a byte-exact restart at the end |
| `test_adversarial` | hostile free text, hostile identifiers, absurd quantities, paths beyond the legacy limit, reserved device names, a file where a directory is expected, impossible enum values, non-canonical token payloads |
| `test_cli` | every command end to end as a real child process, including usage errors, refusals, and exit codes |

Seeds for the randomized suite are printed by the harness and fixed in the source
(`1`, `2`, `3`, `20260214`, `99991`), so any failure is reproducible from the log alone.

### Multiprocess, crash, and recovery evidence

* Writer-lock exclusion and kernel lock release are proven with a real second process that holds
  the lock and is then terminated abruptly; the parent's retry succeeds only after the death.
* Crash consistency is proven by terminating a child process, with `std::_Exit` (no unwinding,
  no destructors, no crash dialog), at each of `before_append`, `after_append_before_flush`,
  `after_flush_before_manifest`, `after_manifest_publish`, and
  `after_segment_rotate_before_manifest`. After every crash the store reopens, its recovered
  state digest matches the manifest, the commit sequence is either unchanged or advanced by
  exactly one, and a further mutation commits successfully.
* Rollback detection is proven by restoring an older, internally consistent pair of manifest
  slots over a newer log and fence; recovery refuses with `rollback_detected` rather than
  silently serving the older state.

### Packaging and downstream proof

* Staged install produced `bin/entl`, the public headers, `lib/resource_entitlement.lib`, and
  the CMake package files.
* `tests/downstream` configured with `find_package(ResourceEntitlement CONFIG REQUIRED)` against
  the installed prefix only, built without warnings, and its single test passed.
* The installed `entl` was exercised directly: `selftest` reported all checks true and `bench`
  completed.
* Fresh-clone closure re-runs configure, build, test, install, and the downstream consumer from
  a clean clone of the release commit.

## Hardening defects found and fixed

These were found by the validation above, not by inspection, and each is fixed at its root
cause with the corresponding regression test in the suite.

1. **Live commits never populated the durable idempotency index.** Only journal replay did, so a
   retried grant after a lost response minted a second entitlement. The index is now written by
   the commit path and by replay through one shared function, which also keeps the recovered
   state digest equal to the committed one.
2. **The in-memory manifest digest went stale after a commit.** `publish_manifest` recorded a
   placeholder digest in its in-memory copy, so the *next* publication wrote a predecessor
   digest that no reader could verify and every reopen after the second commit failed. The
   in-memory copy now carries exactly the digest sealed into the bytes that were written and
   read back.
3. **Fence slots were written at the wrong size.** `FenceRecord::encode` produced a full
   file-sized record rather than one slot, so alternating slot writes grew the fence file past
   its own read bound and every open failed. Slots and the file are now sized and validated
   independently.
4. **Recovered state lost the authority snapshot when no snapshot file existed.** Replay rebuilt
   records but started from an empty authority, so the recovered authority revision could never
   match the manifest and any store without a snapshot was unopenable. The revision, control
   epoch, and mint ordinal are now seeded from the manifest, which is exactly the state the
   first commit published.
5. **Snapshot compaction republished a manifest at an unchanged commit sequence,** so both slots
   could hold equal commit sequences with different content and recovery could not order them.
   Manifests now carry an independent, strictly increasing publication counter.
6. **The committed journal range was not bounded by the manifest.** Recovery decoded bytes past
   the published offset, so an interrupted commit's partial record was treated as corruption
   instead of an uncommitted tail, and a store that had merely crashed could not be opened.
   Recovery now bounds the active segment by the manifest's committed offset.
7. **Replay required at least one applied record.** A store whose snapshot already covered every
   committed record was rejected as corrupt. Replay now distinguishes "nothing left to apply
   because the snapshot is current" from "the records are missing".
8. **Free text was not validated before being persisted.** A note containing a control character
   was written successfully and then rejected by the strict reader on the next open, turning a
   caller mistake into an unreadable ledger. All persisted text now passes one validation choke
   point, and every post-image must round trip byte for byte before a commit is accepted.
9. **A mutation whose decision instant preceded the record's creation instant was persisted** and
   then rejected by the reader. It is now refused with `invalid_timestamp`.
10. **Store paths longer than the legacy Windows limit failed.** Directory creation, existence
    checks, enumeration, and file operations all route through the extended path namespace, so
    long paths work and a component that happens to be a legacy device name is treated as an
    ordinary name instead of being silently redirected.
11. **Merging two independent grants was impossible.** Requiring an identical lineage root made
    merge usable only for split descendants, and requiring identical admission evidence made it
    unusable in practice. Merge now requires identical authority generations, scope, unit,
    priority, and fence mask, and the lineage view is the connected component of the derivation
    graph rather than a single-root filter, so both sources' evidence remains reachable.
12. **AddressSanitizer found a stack-use-after-scope in the test comparison macros.** Binding a
    reference to a value extracted from a temporary `Result` dangled immediately. Both operands
    are now captured by value. This was a defect in the test infrastructure, and it is recorded
    here because it was a real use-after-scope that the sanitizer caught.
13. **Statistics reported a stale journal segment count** that never reflected segment rotation
    during the current session. The count now comes from the writer's live rotation state.

## Benchmarks

### Methodology

`entl bench` measures **completed** operations only. Every mutating measurement includes the
full durable path: journal append, flush to stable storage, byte-identical read-back
verification, dual-slot manifest publication with its own flush and read-back, and the rollback
fence advance. Nothing is reported from a queue or a submission path, because the runtime has
neither. Verification is measured as what it is: an in-memory read of the committed state at a
known commit sequence. Compaction and reopen are measured as single completed operations.

The benchmark creates a fresh store in the system temporary directory, publishes one
authoritative snapshot, then performs the requested grants, verifications, and draws in
sequence, compacts, closes, and reopens. It prints the store directory, the final commit
sequence, the commit sequence reported after reopen, the segment counts, and the log size, so
the run is reproducible. Mean microseconds per completed operation are arithmetic means over a
single run per configuration; no warm-up beyond the first operation is discarded, and no
outlier trimming is applied.

### Measured results

Single host, single NUMA node, local NTFS volume, warm file cache, Release build, MSVC
19.44.35222.0. **SYNTHETIC**: these are filesystem and CPU measurements of this implementation
on this host; they are not measurements of physical hardware behaviour, network storage, or any
other deployment.

| Configuration | Operation (completed) | Count | Mean |
| --- | --- | ---: | ---: |
| 200 entitlements | grant commit | 200 | 14.53 ms |
| 200 entitlements | verify | 20,000 | 1.34 µs |
| 200 entitlements | draw commit | 200 | 15.97 ms |
| 200 entitlements | compaction | 1 | 15.90 ms |
| 200 entitlements | reopen and full recovery | 1 | 21.02 ms |
| 1,000 entitlements (900 preloaded) | grant commit | 100 | 28.57 ms |
| 1,000 entitlements | verify | 5,000 | 2.17 µs |
| 1,000 entitlements | draw commit | 100 | 16.24 ms |
| 1,000 entitlements (900 preloaded) | compaction | 1 | 23.41 ms |
| 1,000 entitlements (900 preloaded) | reopen and full recovery | 1 | 30.17 ms |

Interpretation, stated plainly:

* **Verification is microseconds** and does not touch the disk; it is the hot path and it is
  cheap.
* **A durable commit costs 14–29 ms on this host.** Three flushes dominate (segment, manifest,
  fence); the rest is the byte-identical read-backs and the digest of the whole committed state.
* **Commit cost grows with the number of entitlements**, because the manifest binds the digest
  of the entire committed state. That is a deliberate integrity trade: the head digest proves
  that the recovered state equals the state the last commit produced. On this host the effect
  is visible (14.5 ms at 200 records, 28.6 ms at 1,000) and is reported rather than hidden.
* These figures were not compared against a baseline, so **no speedup or regression claim is
  made**. They are absolute measurements of the current implementation on one host.

## Unsupported and unvalidated behaviour

The following is stated explicitly so that nothing here is over-claimed.

* **One platform was validated.** Everything above was produced on 64-bit Windows 11 with MSVC
  19.44.35222.0 and CMake 4.3.2. The POSIX code paths (flock, `fdatasync`, directory fsync,
  `posix_spawn`) are implemented and compile-gated but were **not** built or executed on this
  host.
* **No cross-machine, network-filesystem, or clustered-filesystem validation was performed.**
  The single-writer lock and the flush/rename primitives assume a local filesystem whose
  advisory locks and flush semantics the operating system honours. Network filesystems commonly
  do not; do not place a store on one without independent validation.
* **No hardware-specific behaviour is claimed.** No accelerator, network device, PDU, or sensor
  was present or exercised. Capacity, bandwidth, power, and placement behaviour belongs to the
  repositories that own those concerns.
* **Sanitizer coverage is AddressSanitizer on MSVC only.** Undefined-behaviour sanitizer and
  thread sanitizer runs were not performed; the library is written to avoid the patterns they
  would look for, but that is an argument, not a measurement.
* **Crash injection is deterministic at five named commit stages.** It does not enumerate every
  instruction boundary, and it does not model power loss or storage-controller reordering beyond
  the flush primitives the operating system provides.
* **The rollback fence is a witness inside the store directory.** It detects a directory rolled
  back to an older consistent state, and it detects a manifest restored without its fence. It
  cannot detect an adversary who restores the entire directory, fence included, to a
  self-consistent older state; that requires an external monotonic witness, which is out of
  scope.
* **Compilation with GCC or Clang was not performed.** The strict warning sets for those
  compilers are configured in CMake and are part of the project's quality policy, but they were
  not executed here.
* **The lost-response window is bounded by configuration.** Authority creation replayed outside
  the window is a new decision; size the window to the retry horizon your deployment needs.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
