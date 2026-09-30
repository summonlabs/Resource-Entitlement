// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal deterministic JSON writer for command output. No third-party code.

#ifndef RESOURCE_ENTITLEMENT_CLI_JSON_HPP
#define RESOURCE_ENTITLEMENT_CLI_JSON_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace entl::cli {

class JsonWriter {
public:
  JsonWriter& begin_object();
  JsonWriter& end_object();
  JsonWriter& begin_array();
  JsonWriter& end_array();
  JsonWriter& key(std::string_view name);

  JsonWriter& value(std::string_view text);
  JsonWriter& value(const char* text) { return value(std::string_view(text == nullptr ? "" : text)); }
  JsonWriter& value(const std::string& text) { return value(std::string_view(text)); }
  JsonWriter& value(bool flag);
  JsonWriter& value(int number) { return value(static_cast<long long>(number)); }
  JsonWriter& value(unsigned number) { return value(static_cast<unsigned long long>(number)); }
  JsonWriter& value(long long number);
  JsonWriter& value(unsigned long long number);

  /// Renders a finite measurement with six fractional digits. Non-finite values
  /// are refused by rendering as 0 rather than emitting invalid JSON.
  JsonWriter& value(double number);

  JsonWriter& null_value();

  [[nodiscard]] const std::string& str() const noexcept { return out_; }

private:
  void separate();
  void push(std::string_view text);

  std::string out_;
  std::vector<bool> first_;
};

/// Escapes a byte string as a JSON string body (without surrounding quotes).
[[nodiscard]] std::string json_escape(std::string_view text);

}  // namespace entl::cli

#endif  // RESOURCE_ENTITLEMENT_CLI_JSON_HPP
