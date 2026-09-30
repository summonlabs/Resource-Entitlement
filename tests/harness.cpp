// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "harness.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace re_test {
namespace {

bool g_case_failed = false;
int g_case_checks = 0;

}  // namespace

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(std::string suite, std::string name, std::function<void()> body) {
  cases_.push_back(TestCase{std::move(suite), std::move(name), std::move(body)});
}

Registrar::Registrar(const char* suite, const char* name, std::function<void()> body) {
  Registry::instance().add(suite, name, std::move(body));
}

void record_failure(const char* file, int line, const std::string& message) {
  g_case_failed = true;
  std::cout << "    " << file << ":" << line << ": " << message << "\n";
}

void record_check() noexcept { ++g_case_checks; }

bool current_case_failed() noexcept { return g_case_failed; }

void begin_case() noexcept {
  g_case_failed = false;
  g_case_checks = 0;
}

bool run_case(const TestCase& test) {
  begin_case();
  const auto start = std::chrono::steady_clock::now();
  bool aborted = false;
  try {
    test.body();
  } catch (const AbortTest& abort) {
    aborted = true;
    record_failure("<harness>", 0, std::string("aborted: ") + abort.what());
  } catch (const std::exception& error) {
    aborted = true;
    record_failure("<harness>", 0, std::string("unexpected exception: ") + error.what());
  } catch (...) {
    aborted = true;
    record_failure("<harness>", 0, "unexpected non-standard exception");
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  const bool passed = !g_case_failed && !aborted;
  std::cout << (passed ? "[ PASS ] " : "[ FAIL ] ") << test.suite << "." << test.name << " ("
            << g_case_checks << " checks, " << elapsed << " ms)\n";
  return passed;
}

int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    } else if (argument == "--list") {
      list_only = true;
    }
  }
  int failed = 0;
  int ran = 0;
  for (const TestCase& test : Registry::instance().cases()) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    if (list_only) {
      std::cout << full << "\n";
      continue;
    }
    ++ran;
    if (!run_case(test)) {
      ++failed;
    }
  }
  if (list_only) {
    return 0;
  }
  std::cout << "\n" << (ran - failed) << "/" << ran << " test cases passed\n";
  if (failed != 0) {
    std::cout << failed << " test case(s) FAILED\n";
    return 1;
  }
  return 0;
}

}  // namespace re_test

int main(int argc, char** argv) { return re_test::run_all(argc, argv); }
