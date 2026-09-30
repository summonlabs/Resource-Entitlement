// Resource Entitlement — commit events.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_EVENTS_HPP
#define RESOURCE_ENTITLEMENT_EVENTS_HPP

#include <cstdint>
#include <functional>
#include <string>

#include "resource_entitlement/error.hpp"
#include "resource_entitlement/strong.hpp"
#include "resource_entitlement/time.hpp"

namespace entl {

enum class EventKind : std::uint8_t {
  kStoreOpened = 1,
  kAuthorityPublished = 2,
  kFencedOnOpen = 3,
  kGranted = 4,
  kSuspended = 5,
  kResumed = 6,
  kRevoked = 7,
  kExpired = 8,
  kMoved = 9,
  kSplit = 10,
  kDelegated = 11,
  kMerged = 12,
  kReissued = 13,
  kDrawn = 14,
  kReleased = 15,
  kCompacted = 16,
};

[[nodiscard]] const char* event_kind_name(EventKind kind) noexcept;

/// A committed fact. Events are emitted only after the durable commit point, so
/// a delivered event always describes state that survives a crash.
struct Event {
  EventKind kind{EventKind::kStoreOpened};
  Sequence commit_seq{};
  Timestamp at{};
  EntitlementId id{};
  Revision revision{};
  ErrorCode code{ErrorCode::kOk};
  std::string detail{};
};

/// Invoked after the commit that produced the event has been published and the
/// store's internal lock has been released. Sinks must be thread-safe if the
/// store is used from more than one thread, and must not call back into the
/// same store instance.
using EventSink = std::function<void(const Event&)>;

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_EVENTS_HPP
