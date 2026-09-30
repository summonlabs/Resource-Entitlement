// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "resource_entitlement/time.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace entl {
namespace {

constexpr std::int64_t kNanosPerSecond = 1000000000LL;
constexpr std::int64_t kSecondsPerDay = 86400LL;

/// Days from 1970-01-01 to the given proleptic Gregorian date.
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t year, int month, int day) noexcept {
  year -= (month <= 2) ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto year_of_era = static_cast<unsigned>(year - era * 400);
  const auto day_of_year =
      static_cast<unsigned>((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
  const unsigned day_of_era = (year_of_era * 365u) + (year_of_era / 4u) - (year_of_era / 100u) + day_of_year;
  return (era * 146097) + static_cast<std::int64_t>(day_of_era) - 719468;
}

struct CivilDate {
  std::int64_t year{1970};
  int month{1};
  int day{1};
};

[[nodiscard]] constexpr CivilDate civil_from_days(std::int64_t days) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - (day_of_era / 1460u) + (day_of_era / 36524u) - (day_of_era / 146096u)) / 365u;
  const std::int64_t year = static_cast<std::int64_t>(year_of_era) + (era * 400);
  const unsigned day_of_year = day_of_era - ((365u * year_of_era) + (year_of_era / 4u) - (year_of_era / 100u));
  const unsigned month_prime = ((5u * day_of_year) + 2u) / 153u;
  const unsigned day = day_of_year - (((153u * month_prime) + 2u) / 5u) + 1u;
  const int month = static_cast<int>(month_prime) + (month_prime < 10u ? 3 : -9);
  return CivilDate{year + (month <= 2 ? 1 : 0), month, static_cast<int>(day)};
}

[[nodiscard]] constexpr bool is_leap_year(std::int64_t year) noexcept {
  return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

[[nodiscard]] constexpr int days_in_month(std::int64_t year, int month) noexcept {
  constexpr std::array<int, 12> kLengths = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return kLengths[static_cast<std::size_t>(month - 1)];
}

[[nodiscard]] bool read_digits(std::string_view text, std::size_t offset, std::size_t count,
                               std::int64_t& out) noexcept {
  if (text.size() < offset + count) {
    return false;
  }
  std::int64_t value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[offset + i];
    if (c < '0' || c > '9') {
      return false;
    }
    value = (value * 10) + (c - '0');
  }
  out = value;
  return true;
}

void append_two_digits(std::string& out, int value) {
  out.push_back(static_cast<char>('0' + ((value / 10) % 10)));
  out.push_back(static_cast<char>('0' + (value % 10)));
}

void append_four_digits(std::string& out, std::int64_t value) {
  out.push_back(static_cast<char>('0' + ((value / 1000) % 10)));
  out.push_back(static_cast<char>('0' + ((value / 100) % 10)));
  out.push_back(static_cast<char>('0' + ((value / 10) % 10)));
  out.push_back(static_cast<char>('0' + (value % 10)));
}

}  // namespace

std::optional<Timestamp> Timestamp::from_unix_nanos(std::int64_t nanos) noexcept {
  if (nanos < 0) {
    return std::nullopt;
  }
  Timestamp result;
  result.nanos_ = nanos;
  return result;
}

std::optional<Timestamp> Timestamp::parse(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }

  bool all_digits = true;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      all_digits = false;
      break;
    }
  }
  if (all_digits) {
    if (text.size() > 19u) {
      return std::nullopt;
    }
    std::int64_t value = 0;
    if (!read_digits(text, 0, text.size(), value)) {
      return std::nullopt;
    }
    return from_unix_nanos(value);
  }

  if (text.size() < 20u) {
    return std::nullopt;
  }
  const char separator = text[10];
  if (separator != 'T' && separator != 't' && separator != ' ') {
    return std::nullopt;
  }
  if (text[4] != '-' || text[7] != '-' || text[13] != ':' || text[16] != ':') {
    return std::nullopt;
  }

  std::int64_t year = 0;
  std::int64_t month = 0;
  std::int64_t day = 0;
  std::int64_t hour = 0;
  std::int64_t minute = 0;
  std::int64_t second = 0;
  if (!read_digits(text, 0, 4, year) || !read_digits(text, 5, 2, month) || !read_digits(text, 8, 2, day) ||
      !read_digits(text, 11, 2, hour) || !read_digits(text, 14, 2, minute) || !read_digits(text, 17, 2, second)) {
    return std::nullopt;
  }

  std::size_t position = 19;
  std::int64_t fraction = 0;
  if (position < text.size() && text[position] == '.') {
    ++position;
    const std::size_t digits_begin = position;
    std::int64_t value = 0;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
      if (position - digits_begin >= 9u) {
        return std::nullopt;
      }
      value = (value * 10) + (text[position] - '0');
      ++position;
    }
    const std::size_t digits = position - digits_begin;
    if (digits == 0u) {
      return std::nullopt;
    }
    for (std::size_t i = digits; i < 9u; ++i) {
      value *= 10;
    }
    fraction = value;
  }
  if (position >= text.size()) {
    return std::nullopt;
  }
  if (text[position] != 'Z' && text[position] != 'z') {
    return std::nullopt;
  }
  ++position;
  if (position != text.size()) {
    return std::nullopt;
  }

  if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 ||
      day > days_in_month(year, static_cast<int>(month)) || hour > 23 || minute > 59 || second > 59) {
    return std::nullopt;
  }

  const std::int64_t days = days_from_civil(year, static_cast<int>(month), static_cast<int>(day));
  const std::int64_t total_seconds = (days * kSecondsPerDay) + (hour * 3600) + (minute * 60) + second;
  if (total_seconds < 0 || total_seconds > (kMaxUnixNanos / kNanosPerSecond)) {
    return std::nullopt;
  }
  return from_unix_nanos((total_seconds * kNanosPerSecond) + fraction);
}

std::string Timestamp::to_rfc3339() const {
  const std::int64_t total_seconds = nanos_ / kNanosPerSecond;
  const std::int64_t fraction = nanos_ % kNanosPerSecond;
  const std::int64_t days = total_seconds / kSecondsPerDay;
  const std::int64_t second_of_day = total_seconds % kSecondsPerDay;
  const CivilDate date = civil_from_days(days);

  std::string out;
  out.reserve(30);
  append_four_digits(out, date.year);
  out.push_back('-');
  append_two_digits(out, date.month);
  out.push_back('-');
  append_two_digits(out, date.day);
  out.push_back('T');
  append_two_digits(out, static_cast<int>(second_of_day / 3600));
  out.push_back(':');
  append_two_digits(out, static_cast<int>((second_of_day % 3600) / 60));
  out.push_back(':');
  append_two_digits(out, static_cast<int>(second_of_day % 60));
  out.push_back('.');
  std::int64_t remainder = fraction;
  std::array<char, 9> digits{};
  for (std::size_t i = 0; i < 9u; ++i) {
    digits[8u - i] = static_cast<char>('0' + (remainder % 10));
    remainder /= 10;
  }
  out.append(digits.data(), digits.size());
  out.push_back('Z');
  return out;
}

std::optional<Timestamp> Timestamp::checked_add_nanos(std::int64_t delta) const noexcept {
  if (delta > 0 && nanos_ > (kMaxUnixNanos - delta)) {
    return std::nullopt;
  }
  if (delta < 0 && nanos_ < -delta) {
    return std::nullopt;
  }
  return from_unix_nanos(nanos_ + delta);
}

std::optional<std::int64_t> Timestamp::checked_distance_nanos(const Timestamp& later) const noexcept {
  return later.nanos_ - nanos_;
}

Timestamp SystemClock::now() const {
  const auto point = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(point).count();
  return Timestamp::from_unix_nanos(nanos).value_or(Timestamp{});
}

std::string format_nanos(std::int64_t nanos) {
  const bool negative = nanos < 0;
  const std::uint64_t magnitude = negative ? static_cast<std::uint64_t>(-(nanos + 1)) + 1u
                                           : static_cast<std::uint64_t>(nanos);
  std::string digits = std::to_string(magnitude);
  std::string sign = negative ? "-" : "";
  if (magnitude < 1000u) {
    return sign + digits + " ns";
  }
  if (magnitude < 1000000u) {
    return sign + std::to_string(magnitude / 1000u) + "." + std::to_string((magnitude % 1000u) / 100u) + " us";
  }
  if (magnitude < 1000000000u) {
    return sign + std::to_string(magnitude / 1000000u) + "." + std::to_string((magnitude % 1000000u) / 100000u) + " ms";
  }
  return sign + std::to_string(magnitude / 1000000000u) + "." +
         std::to_string((magnitude % 1000000000u) / 100000000u) + " s";
}

}  // namespace entl
