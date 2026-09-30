// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// End-to-end command line tests. Every command runs as a real child process.

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

#include "harness.hpp"
#include "test_support.hpp"

namespace {

struct CliResult {
  int exit_code{-1};
  std::string output{};
  bool started{false};
};

std::filesystem::path cli_executable() { return re_test::executable_from_env("ENTL_CLI"); }

CliResult run_cli(const std::vector<std::string>& arguments) {
  const std::filesystem::path executable = cli_executable();
  if (executable.empty()) {
    return CliResult{};
  }
  const re_test::ProcessResult result = re_test::run_process(executable, arguments);
  CliResult cli;
  cli.started = result.started;
  cli.exit_code = result.exit_code;
  cli.output = result.output;
  return cli;
}

/// One authoritative instant shared by every command in this suite. A mutation
/// evaluated at an instant earlier than the record's creation is a caller
/// error, so the tests must not mix the system clock with fixed instants.
constexpr const char* kNow = "2026-02-01T00:00:00Z";

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string field(const std::string& json, const std::string& key) {
  const std::string pattern = "\"" + key + "\":";
  const std::size_t at = json.find(pattern);
  if (at == std::string::npos) {
    return {};
  }
  std::size_t cursor = at + pattern.size();
  if (cursor >= json.size()) {
    return {};
  }
  if (json[cursor] == '"') {
    ++cursor;
    const std::size_t end = json.find('"', cursor);
    return end == std::string::npos ? std::string() : json.substr(cursor, end - cursor);
  }
  const std::size_t end = json.find_first_of(",}", cursor);
  return end == std::string::npos ? json.substr(cursor) : json.substr(cursor, end - cursor);
}

void write_request(const std::filesystem::path& path, const std::vector<std::string>& lines) {
  std::ofstream stream(path, std::ios::trunc);
  for (const std::string& line : lines) {
    stream << line << "\n";
  }
}

std::vector<std::string> standard_request_lines() {
  return {
      "tenant=tenant-a",
      "service=inference",
      "service-class=gold",
      "facility=dc-1",
      "resource-type=accelerator",
      "scope=pool-a",
      "unit=count",
      "quantity=100",
      "priority-class=standard",
      "priority-rank=0",
      "effective-from=2026-01-01T00:00:00Z",
      "expires-at=2027-01-01T00:00:00Z",
      "admission-digest=3333333333333333333333333333333333333333333333333333333333333333",
      "actor=operator-1",
  };
}

struct CliFixture {
  explicit CliFixture(std::string_view hint) : directory(hint) {
    const CliResult init = run_cli({"init", "--dir", directory.string()});
    ok = init.started && init.exit_code == 0 && contains(init.output, "\"ok\":true");
  }

  re_test::TempDirectory directory;
  bool ok{false};
  std::string last_grant_id;

  bool publish_authority() {
    const CliResult result = run_cli({"authority", "--dir", directory.string(), "--publisher",
                                      "facility-capacity", "--capacity-generation", "1",
                                      "--policy-revision", "1", "--envelope-revision", "1",
                                      "--service-class-revision", "1", "--now", kNow});
    return result.exit_code == 0;
  }

  bool grant(std::string_view extra_key = {}, std::string_view extra_value = {}) {
    std::vector<std::string> lines = standard_request_lines();
    if (!extra_key.empty()) {
      lines.push_back(std::string(extra_key) + "=" + std::string(extra_value));
    }
    return grant_from(lines);
  }

  bool grant_from(const std::vector<std::string>& lines) {
    const std::filesystem::path request = directory.path() / "grant.txt";
    write_request(request, lines);
    const CliResult result =
        run_cli({"grant", "--dir", directory.string(), "--request", request.string(), "--now", kNow});
    if (result.exit_code == 0) {
      last_grant_id = field(result.output, "id");
    }
    return result.exit_code == 0;
  }
};

}  // namespace

RE_TEST(cli, version_and_selftest_report_machine_readable_success) {
  const CliResult version = run_cli({"version"});
  RE_REQUIRE(version.started);
  RE_CHECK_EQ(version.exit_code, 0);
  RE_CHECK(contains(version.output, "\"ok\":true"));
  RE_CHECK(contains(version.output, "\"version\":\"1.0.0\""));

  const CliResult selftest = run_cli({"selftest"});
  RE_REQUIRE(selftest.started);
  RE_CHECK_EQ(selftest.exit_code, 0);
  RE_CHECK(contains(selftest.output, "\"sha256_vectors\":3"));
  RE_CHECK(contains(selftest.output, "\"crc32c_vector\":true"));
  RE_CHECK(contains(selftest.output, "\"canonical_round_trip\":true"));
}

RE_TEST(cli, init_authority_grant_and_read_back) {
  CliFixture fixture("cli-lifecycle");
  RE_REQUIRE(fixture.ok);
  RE_REQUIRE(fixture.publish_authority());
  RE_REQUIRE(fixture.grant());
  RE_CHECK_EQ(fixture.last_grant_id.size(), std::size_t{32});

  const CliResult show = run_cli({"show", "--dir", fixture.directory.string(), "--id", fixture.last_grant_id});
  RE_CHECK_EQ(show.exit_code, 0);
  RE_CHECK(contains(show.output, "\"state\":\"active\""));
  RE_CHECK(contains(show.output, "\"tenant-a\""));

  const CliResult listing = run_cli({"list", "--dir", fixture.directory.string(), "--now",
                                     "2026-02-01T00:00:00Z", "--live"});
  RE_CHECK_EQ(listing.exit_code, 0);
  RE_CHECK(contains(listing.output, "\"count\":1"));

  const CliResult verify = run_cli({"verify", "--dir", fixture.directory.string(), "--id",
                                    fixture.last_grant_id, "--now", "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(verify.exit_code, 0);
  RE_CHECK(contains(verify.output, "\"authorized\":true"));

  const CliResult over = run_cli({"verify", "--dir", fixture.directory.string(), "--id",
                                  fixture.last_grant_id, "--now", "2026-02-01T00:00:00Z", "--quantity",
                                  "1000", "--unit", "count"});
  RE_CHECK_EQ(over.exit_code, 1);
  RE_CHECK(contains(over.output, "\"authorized\":false"));
  RE_CHECK(contains(over.output, "quantity_exceeded"));

  const CliResult stats = run_cli({"stats", "--dir", fixture.directory.string()});
  RE_CHECK_EQ(stats.exit_code, 0);
  RE_CHECK(contains(stats.output, "\"entitlement_count\":1"));

  const CliResult inspect = run_cli({"inspect", "--dir", fixture.directory.string()});
  RE_CHECK_EQ(inspect.exit_code, 0);
  RE_CHECK(contains(inspect.output, "\"rollback_detected\":false"));
  RE_CHECK(contains(inspect.output, "\"manifest_slots_consistent\":true"));
}

RE_TEST(cli, lifecycle_commands_drive_state_and_exit_codes) {
  CliFixture fixture("cli-transitions");
  RE_REQUIRE(fixture.ok);
  RE_REQUIRE(fixture.publish_authority());
  RE_REQUIRE(fixture.grant());

  const CliResult draw = run_cli({"draw", "--dir", fixture.directory.string(), "--id",
                                  fixture.last_grant_id, "--revision", "1", "--quantity", "30", "--unit",
                                  "count", "--actor", "operator-1", "--now", "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(draw.exit_code, 0);
  RE_CHECK(contains(draw.output, "\"remaining\":{\"unit\":\"count\",\"units\":70}"));

  const CliResult suspend = run_cli({"suspend", "--dir", fixture.directory.string(), "--id",
                                     fixture.last_grant_id, "--revision", "2", "--actor", "operator-1",
                                     "--now", "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(suspend.exit_code, 0);

  const CliResult suspended_verify = run_cli({"verify", "--dir", fixture.directory.string(), "--id",
                                              fixture.last_grant_id, "--now", "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(suspended_verify.exit_code, 1);
  RE_CHECK(contains(suspended_verify.output, "suspended"));

  const CliResult resume = run_cli({"resume", "--dir", fixture.directory.string(), "--id",
                                    fixture.last_grant_id, "--revision", "3", "--actor", "operator-1",
                                    "--now", "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(resume.exit_code, 0);

  const CliResult revoke = run_cli({"revoke", "--dir", fixture.directory.string(), "--id",
                                    fixture.last_grant_id, "--revision", "4", "--actor", "operator-1",
                                    "--reason", "policy", "--now", "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(revoke.exit_code, 0);

  const CliResult revoked_verify = run_cli({"verify", "--dir", fixture.directory.string(), "--id",
                                            fixture.last_grant_id, "--now", "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(revoked_verify.exit_code, 1);
  RE_CHECK(contains(revoked_verify.output, "\"code\":\"revoked\""));

  // A stale revision is a refusal, not a usage error.
  const CliResult stale = run_cli({"suspend", "--dir", fixture.directory.string(), "--id",
                                   fixture.last_grant_id, "--revision", "4", "--actor", "operator-1",
                                   "--now", "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(stale.exit_code, 1);
  RE_CHECK(contains(stale.output, "revision_conflict"));
}

RE_TEST(cli, tokens_transfers_lineage_and_ordering) {
  CliFixture fixture("cli-graph");
  RE_REQUIRE(fixture.ok);
  RE_REQUIRE(fixture.publish_authority());
  RE_REQUIRE(fixture.grant());

  const CliResult token = run_cli({"token", "--dir", fixture.directory.string(), "--id",
                                   fixture.last_grant_id});
  RE_CHECK_EQ(token.exit_code, 0);
  const std::string token_hex = field(token.output, "token_hex");
  RE_CHECK(!token_hex.empty());

  const CliResult verify_token = run_cli({"verify-token", "--dir", fixture.directory.string(), "--token",
                                          token_hex, "--now", "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(verify_token.exit_code, 0);
  RE_CHECK(contains(verify_token.output, "\"authorized\":true"));

  const CliResult corrupted = run_cli({"verify-token", "--dir", fixture.directory.string(), "--token",
                                       std::string("00") + token_hex});
  RE_CHECK_EQ(corrupted.exit_code, 2);

  const CliResult split = run_cli({"transfer", "--dir", fixture.directory.string(), "--id",
                                   fixture.last_grant_id, "--revision", "1", "--mode", "split", "--target",
                                   "tenant-b", "--quantity", "40", "--unit", "count", "--actor",
                                   "operator-1", "--now", "2026-02-01T00:00:00Z", "--priority-class",
                                   "standard"});
  RE_CHECK_EQ(split.exit_code, 0);
  const std::string child = field(split.output, "id");
  RE_CHECK_EQ(child.size(), std::size_t{32});

  const CliResult order = run_cli({"order", "--dir", fixture.directory.string(), "--now",
                                   "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(order.exit_code, 0);
  RE_CHECK(contains(order.output, "advisory ordering only"));

  const CliResult lineage = run_cli({"lineage", "--dir", fixture.directory.string(), "--id", child});
  RE_CHECK_EQ(lineage.exit_code, 0);
  RE_CHECK(contains(lineage.output, "\"relation\":\"split_from\""));
  RE_CHECK(contains(lineage.output, "\"nodes\":["));

  const CliResult revoke_child = run_cli({"revoke", "--dir", fixture.directory.string(), "--id", child,
                                          "--revision", "1", "--actor", "operator-1", "--now",
                                          "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(revoke_child.exit_code, 0);
  const CliResult after = run_cli({"lineage", "--dir", fixture.directory.string(), "--id", child});
  RE_CHECK(contains(after.output, "\"live\":false"));
}

RE_TEST(cli, merge_reissue_compact_and_bench) {
  CliFixture fixture("cli-merge");
  RE_REQUIRE(fixture.ok);
  RE_REQUIRE(fixture.publish_authority());
  RE_REQUIRE(fixture.grant());
  const std::string first = fixture.last_grant_id;
  // A distinct request document is a distinct request identity; an identical
  // one would correctly resolve as a replay of the first grant.
  RE_REQUIRE(fixture.grant("note", "the second independent grant"));
  const std::string second = fixture.last_grant_id;
  RE_CHECK_NE(first, second);

  const CliResult merge = run_cli({"merge", "--dir", fixture.directory.string(), "--first", first,
                                   "--first-revision", "1", "--second", second, "--second-revision", "1",
                                   "--actor", "operator-1", "--now", "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(merge.exit_code, 0);
  const std::string merged = field(merge.output, "id");

  const CliResult reissue = run_cli({"reissue", "--dir", fixture.directory.string(), "--id", merged,
                                     "--revision", "1", "--actor", "operator-1", "--now",
                                     "2026-02-01T00:00:00Z"});
  RE_CHECK_EQ(reissue.exit_code, 0);

  const CliResult compact = run_cli({"compact", "--dir", fixture.directory.string()});
  RE_CHECK_EQ(compact.exit_code, 0);
  RE_CHECK(contains(compact.output, "\"performed\":true"));

  const CliResult bench = run_cli({"bench", "--grants", "5", "--verifies", "20", "--draws", "2"});
  RE_CHECK_EQ(bench.exit_code, 0);
  RE_CHECK(contains(bench.output, "\"provenance\":\"SYNTHETIC\""));
  RE_CHECK(contains(bench.output, "mean_microseconds_per_completed_operation"));
  RE_CHECK(contains(bench.output, "commit_seq_after_reopen"));
}

RE_TEST(cli, usage_errors_are_distinguished_from_refusals) {
  const CliResult unknown_command = run_cli({"frobnicate"});
  RE_CHECK_EQ(unknown_command.exit_code, 2);
  RE_CHECK(contains(unknown_command.output, "unknown command"));

  const CliResult unknown_option = run_cli({"list", "--dir", ".", "--nonsense", "1"});
  RE_CHECK_EQ(unknown_option.exit_code, 2);
  RE_CHECK(contains(unknown_option.output, "\"code\":\"unexpected_field\""));

  const CliResult missing = run_cli({"list"});
  RE_CHECK_EQ(missing.exit_code, 2);
  RE_CHECK(contains(missing.output, "missing_field"));

  const CliResult no_such_store = run_cli(
      {"stats", "--dir", (std::filesystem::temp_directory_path() / "definitely-not-a-store").string()});
  RE_CHECK_EQ(no_such_store.exit_code, 3);

  re_test::TempDirectory holder("cli-bad-request");
  RE_REQUIRE(run_cli({"init", "--dir", holder.path().string()}).exit_code == 0);

  const CliResult bad_number = run_cli({"show", "--dir", holder.path().string(), "--id", "xyz"});
  RE_CHECK_EQ(bad_number.exit_code, 2);
  RE_CHECK(contains(bad_number.output, "invalid_argument"));
  const std::filesystem::path request = holder.path() / "bad.txt";
  write_request(request, {"tenant=tenant-a", "unknown-key=1"});
  const CliResult unknown_key = run_cli({"grant", "--dir", holder.path().string(), "--request",
                                         request.string()});
  RE_CHECK_EQ(unknown_key.exit_code, 2);
  RE_CHECK(contains(unknown_key.output, "unexpected_field"));

  write_request(request, {"tenant=tenant-a", "tenant=tenant-b"});
  const CliResult duplicate = run_cli({"grant", "--dir", holder.path().string(), "--request",
                                       request.string()});
  RE_CHECK_EQ(duplicate.exit_code, 2);
  RE_CHECK(contains(duplicate.output, "duplicate_field"));

  write_request(request, {"tenant=tenant-a"});
  const CliResult incomplete = run_cli({"grant", "--dir", holder.path().string(), "--request",
                                        request.string()});
  RE_CHECK_EQ(incomplete.exit_code, 2);
  RE_CHECK(contains(incomplete.output, "missing_field"));

  write_request(request, {"tenant=CON", "service=inference", "service-class=gold", "facility=dc-1",
                          "resource-type=accelerator", "scope=pool-a", "unit=count", "quantity=1",
                          "effective-from=2026-01-01T00:00:00Z", "expires-at=2027-01-01T00:00:00Z",
                          "admission-digest=3333333333333333333333333333333333333333333333333333333333333333",
                          "actor=operator-1"});
  const CliResult reserved = run_cli({"grant", "--dir", holder.path().string(), "--request",
                                      request.string()});
  RE_CHECK_EQ(reserved.exit_code, 2);
  RE_CHECK(contains(reserved.output, "invalid_identifier"));

  const CliResult malformed_instant = run_cli({"verify", "--dir", holder.path().string(), "--id",
                                               std::string(32u, '0'), "--now", "not-a-time"});
  RE_CHECK_EQ(malformed_instant.exit_code, 2);
}

RE_TEST(cli, help_and_version_flags_are_supported) {
  const CliResult usage = run_cli({"--help"});
  RE_CHECK_EQ(usage.exit_code, 0);
  RE_CHECK(contains(usage.output, "Usage: entl <command>"));

  const CliResult bare = run_cli({});
  RE_CHECK_EQ(bare.exit_code, 2);

  const CliResult short_version = run_cli({"--version"});
  RE_CHECK_EQ(short_version.exit_code, 0);
  RE_CHECK(contains(short_version.output, "entl 1.0.0"));
}
