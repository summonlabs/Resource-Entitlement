// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared test fixtures: temporary stores, deterministic request builders,
// process spawning, and filesystem helpers.

#ifndef RESOURCE_ENTITLEMENT_TESTS_TEST_SUPPORT_HPP
#define RESOURCE_ENTITLEMENT_TESTS_TEST_SUPPORT_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "harness.hpp"
#include "resource_entitlement/store.hpp"

namespace re_test {

/// A unique directory below the system temporary directory, removed on
/// destruction. Removal tolerates Windows reserved device names and read-only
/// files.
class TempDirectory {
public:
  explicit TempDirectory(std::string_view hint);
  ~TempDirectory();
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;
  TempDirectory(TempDirectory&&) = delete;
  TempDirectory& operator=(TempDirectory&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path child(std::string_view name) const;
  [[nodiscard]] std::string string() const { return path_.string(); }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::filesystem::path system_temp_root();
void remove_tree_force(const std::filesystem::path& path) noexcept;

template <class T>
[[nodiscard]] T must_parse(std::string_view text) {
  auto parsed = T::parse(text);
  if (!parsed.has_value()) {
    throw std::runtime_error("test fixture: could not parse identifier '" + std::string(text) + "'");
  }
  return parsed.value();
}

[[nodiscard]] entl::Timestamp instant(std::int64_t unix_seconds);
[[nodiscard]] entl::Sha256Digest digest_of(std::string_view text);
[[nodiscard]] entl::RequestId request_id(std::string_view seed);
[[nodiscard]] entl::EntitlementId entitlement_id(std::string_view hex);

[[nodiscard]] entl::AuthorityUpdateRequest authority_request(std::string_view seed, entl::Timestamp now,
                                                             entl::Revision expected,
                                                             std::uint64_t capacity_generation);

struct Fixture {
  entl::GrantRequest grant{};
};

/// A store already opened for writing in a fresh temporary directory.
class StoreFixture {
public:
  StoreFixture();
  explicit StoreFixture(const entl::StoreOpenOptions& options);

  [[nodiscard]] entl::Store& store() { return *store_; }
  [[nodiscard]] const std::filesystem::path& path() const { return directory_.path(); }
  [[nodiscard]] entl::Timestamp now() const { return now_; }

  /// Publishes a first real authoritative snapshot and returns the new revision.
  entl::Revision publish_authority(std::uint64_t capacity_generation,
                                   std::uint64_t policy_revision = 1,
                                   std::uint64_t envelope_revision = 1,
                                   std::uint64_t service_class_revision = 1);

  /// Grants one entitlement and returns its identity.
  entl::EntitlementId grant_default(std::string_view id_seed, std::uint64_t units,
                                    std::string_view tenant = "tenant-a",
                                    std::string_view scope = "scope-a");
  entl::EntitlementId grant_full(const entl::GrantRequest& request);

  [[nodiscard]] entl::GrantRequest make_grant(std::string_view id_seed, std::uint64_t units,
                                              std::string_view tenant = "tenant-a",
                                              std::string_view scope = "scope-a") const;

  static constexpr std::uint64_t kMaxCapacityGeneration = 7u;

private:
  TempDirectory directory_;
  std::unique_ptr<entl::Store> store_;
  entl::Timestamp now_ = instant(1700000000);
  std::uint64_t counter_ = 0;
};

/// Convenience: opens a store with the standard test bounds.
[[nodiscard]] entl::StoreOpenOptions test_options(const std::filesystem::path& directory,
                                                  bool create_if_missing = true);

/// Opens (or reopens) a store at an explicit path.
[[nodiscard]] entl::Result<std::unique_ptr<entl::Store>> open_store_at(
    const std::filesystem::path& directory, bool create_if_missing = false, bool fence_on_open = false,
    std::uint32_t idempotency_capacity = 8192u);

struct ProcessResult {
  int exit_code{-1};
  std::string output{};
  bool started{false};
  std::string error{};
};

/// Runs a process to completion with stdout and stderr redirected to a
/// temporary file; never uses pipes.
[[nodiscard]] ProcessResult run_process(const std::filesystem::path& executable,
                                        const std::vector<std::string>& arguments,
                                        const std::vector<std::string>& environment = {});

/// A process started in the background that the test can terminate abruptly.
class ChildProcess {
public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&&) noexcept;
  ChildProcess& operator=(ChildProcess&&) noexcept;

  [[nodiscard]] static std::optional<ChildProcess> start(const std::filesystem::path& executable,
                                                         const std::vector<std::string>& arguments,
                                                         const std::vector<std::string>& environment = {});

  /// Terminates the process without giving it a chance to clean up.
  void terminate_now();
  [[nodiscard]] ProcessResult wait();
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] std::uint64_t pid() const noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
};

[[nodiscard]] std::filesystem::path executable_from_env(const char* name);

/// Reads a whole file as bytes; throws only when the file cannot be read.
[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path);
void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);

}  // namespace re_test

#endif  // RESOURCE_ENTITLEMENT_TESTS_TEST_SUPPORT_HPP
