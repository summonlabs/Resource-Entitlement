// Resource Entitlement — authoritative time semantics.
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_TIME_HPP
#define RESOURCE_ENTITLEMENT_TIME_HPP

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace entl {

/// A non-negative count of nanoseconds since 1970-01-01T00:00:00Z.
///
/// The library never reads a clock implicitly during a decision: every mutating
/// request and every verification carries the decision instant supplied by the
/// caller. Clock is only used by the command line tool to obtain "now" once, at
/// the start of a command.
class Timestamp {
public:
  constexpr Timestamp() noexcept = default;

  /// Decision instants are non-negative; negative values are rejected.
  [[nodiscard]] static std::optional<Timestamp> from_unix_nanos(std::int64_t nanos) noexcept;

  /// Accepts either a bare integer nanosecond count or an RFC 3339 UTC instant:
  /// "YYYY-MM-DDTHH:MM:SS[.fraction]Z" with 0..9 fractional digits, optionally
  /// using a lowercase 't'/'z' separator. Offsets other than Z are rejected so
  /// that the encoding is unambiguous.
  [[nodiscard]] static std::optional<Timestamp> parse(std::string_view text) noexcept;

  [[nodiscard]] constexpr std::int64_t unix_nanos() const noexcept { return nanos_; }

  /// Canonical RFC 3339 rendering with nanosecond precision, e.g.
  /// "2026-02-14T03:04:05.000000007Z". Every value representable by this type
  /// renders exactly; the supported calendar range is 1970-01-01 .. 2262-04-11.
  [[nodiscard]] std::string to_rfc3339() const;

  /// Checked arithmetic: returns nullopt on overflow instead of wrapping.
  [[nodiscard]] std::optional<Timestamp> checked_add_nanos(std::int64_t delta) const noexcept;
  [[nodiscard]] std::optional<std::int64_t> checked_distance_nanos(const Timestamp& later) const noexcept;

  /// Zero means "not set". Callers that require an instant must reject zero
  /// explicitly at their own boundary.
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }

  friend constexpr bool operator==(const Timestamp&, const Timestamp&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const Timestamp&, const Timestamp&) noexcept = default;

  /// Largest value this type can hold.
  static constexpr std::int64_t kMaxUnixNanos = 9223372036854775807LL;

private:
  std::int64_t nanos_{0};
};

/// Injectable time source. Implementations must be thread-safe if shared.
class Clock {
public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;

  [[nodiscard]] virtual Timestamp now() const = 0;
};

/// Wall clock in UTC. Reads the system clock; used by the CLI only.
class SystemClock final : public Clock {
public:
  [[nodiscard]] Timestamp now() const override;
};

/// Deterministic clock for tests and replay.
class ManualClock final : public Clock {
public:
  ManualClock() = default;
  explicit ManualClock(Timestamp start) : current_(start) {}
  [[nodiscard]] Timestamp now() const override { return current_; }
  void set(Timestamp value) noexcept { current_ = value; }
  void advance_nanos(std::int64_t delta) noexcept { current_ = Timestamp::from_unix_nanos(current_.unix_nanos() + delta).value_or(current_); }

private:
  Timestamp current_{};
};

/// Formats a duration in nanoseconds as e.g. "12.345 ms".
[[nodiscard]] std::string format_nanos(std::int64_t nanos);

}  // namespace entl

#endif  // RESOURCE_ENTITLEMENT_TIME_HPP
