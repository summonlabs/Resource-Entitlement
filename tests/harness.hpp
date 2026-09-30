// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal deterministic test harness. No third-party dependency.

#ifndef RESOURCE_ENTITLEMENT_TESTS_HARNESS_HPP
#define RESOURCE_ENTITLEMENT_TESTS_HARNESS_HPP

#include <cstdint>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace re_test {

/// Thrown by RE_REQUIRE to abandon the current test case immediately.
class AbortTest : public std::exception {
public:
  explicit AbortTest(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

private:
  std::string message_;
};

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

class Registry {
public:
  static Registry& instance();

  void add(std::string suite, std::string name, std::function<void()> body);
  [[nodiscard]] const std::vector<TestCase>& cases() const noexcept { return cases_; }

private:
  std::vector<TestCase> cases_;
};

void record_failure(const char* file, int line, const std::string& message);
void record_check() noexcept;
[[nodiscard]] bool current_case_failed() noexcept;
void begin_case() noexcept;

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body);
};

/// Generic value rendering for diagnostics. Types without a dedicated overload
/// render as "<value>" rather than failing to compile.
template <class T>
std::string describe(const T&) {
  return "<value>";
}
inline std::string describe(bool value) { return value ? "true" : "false"; }
inline std::string describe(int value) { return std::to_string(value); }
inline std::string describe(unsigned value) { return std::to_string(value); }
inline std::string describe(long value) { return std::to_string(value); }
inline std::string describe(unsigned long value) { return std::to_string(value); }
inline std::string describe(long long value) { return std::to_string(value); }
inline std::string describe(unsigned long long value) { return std::to_string(value); }
inline std::string describe(std::string_view value) { return std::string(value); }
inline std::string describe(const std::string& value) { return value; }
inline std::string describe(const char* value) { return value == nullptr ? "<null>" : value; }

/// Runs a single test case and reports its result. Returns true when it passed.
bool run_case(const TestCase& test);

/// Runs every registered case; honours --filter=<substring> and --list.
int run_all(int argc, char** argv);

}  // namespace re_test

#define RE_TEST(suite_name, case_name)                                                            static void suite_name##_##case_name##_body();                                                  static const ::re_test::Registrar suite_name##_##case_name##_registrar(                             #suite_name, #case_name, &suite_name##_##case_name##_body);                                 static void suite_name##_##case_name##_body()

// Comparison operands are captured by value. Binding a reference to a value
// extracted from a temporary (for example `store.get(id)->revision`) would
// dangle as soon as that temporary is destroyed; AddressSanitizer reported
// exactly that as a stack-use-after-scope before this was corrected.
#define RE_FAIL(message) ::re_test::record_failure(__FILE__, __LINE__, (message))

#define RE_CHECK(condition)                                                                       do {                                                                                              ::re_test::record_check();                                                                      if (!(condition)) {                                                                               ::re_test::record_failure(__FILE__, __LINE__, "CHECK failed: " #condition);                    }                                                                                             } while (false)

#define RE_REQUIRE(condition)                                                                     do {                                                                                              ::re_test::record_check();                                                                      if (!(condition)) {                                                                               ::re_test::record_failure(__FILE__, __LINE__, "REQUIRE failed: " #condition);                    throw ::re_test::AbortTest("REQUIRE failed: " #condition);                                     }                                                                                             } while (false)

#define RE_CHECK_EQ(actual, expected)                                                             do {                                                                                              ::re_test::record_check();                                                                      const auto re_actual = (actual);                                                               const auto re_expected = (expected);                                                           if (!(re_actual == re_expected)) {                                                                ::re_test::record_failure(__FILE__, __LINE__,                                                                             std::string("CHECK_EQ failed: " #actual " == " #expected) +                                          " (actual=" + ::re_test::describe(re_actual) +                                                   ", expected=" + ::re_test::describe(re_expected) + ")");         }                                                                                             } while (false)

#define RE_CHECK_NE(actual, unexpected)                                                           do {                                                                                              ::re_test::record_check();                                                                      const auto re_actual = (actual);                                                               if (re_actual == (unexpected)) {                                                                  ::re_test::record_failure(__FILE__, __LINE__,                                                                             "CHECK_NE failed: " #actual " != " #unexpected);                     }                                                                                             } while (false)

#define RE_REQUIRE_OK(result_expression)                                                          do {                                                                                              ::re_test::record_check();                                                                      auto re_result = (result_expression);                                                           if (!re_result.has_value()) {                                                                     ::re_test::record_failure(__FILE__, __LINE__,                                                                             std::string("expected success from " #result_expression                                                    " but got ") + re_result.error().to_string());             throw ::re_test::AbortTest("expected success");                                               }                                                                                             } while (false)

#define RE_CHECK_ERR(result_expression, expected_code)                                            do {                                                                                              ::re_test::record_check();                                                                      auto re_result = (result_expression);                                                           if (re_result.has_value()) {                                                                      ::re_test::record_failure(__FILE__, __LINE__,                                                                             "expected a refusal from " #result_expression);                      } else if (re_result.error().code() != (expected_code)) {                                         ::re_test::record_failure(__FILE__, __LINE__,                                                                             std::string("wrong refusal code from " #result_expression                                                   ": got ") + re_result.error().to_string() +                                              " expected " + ::entl::error_code_name(expected_code));          }                                                                                             } while (false)

#define RE_REQUIRE_ERR(result_expression, expected_code)                                          do {                                                                                              ::re_test::record_check();                                                                      auto re_result = (result_expression);                                                           if (re_result.has_value()) {                                                                      ::re_test::record_failure(__FILE__, __LINE__,                                                                             "expected a refusal from " #result_expression);                        throw ::re_test::AbortTest("expected a refusal");                                             }                                                                                               if (re_result.error().code() != (expected_code)) {                                                ::re_test::record_failure(__FILE__, __LINE__,                                                                             std::string("wrong refusal code from " #result_expression                                                   ": got ") + re_result.error().to_string() +                                              " expected " + ::entl::error_code_name(expected_code));            throw ::re_test::AbortTest("wrong refusal code");                                             }                                                                                             } while (false)

#endif  // RESOURCE_ENTITLEMENT_TESTS_HARNESS_HPP
