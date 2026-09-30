// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

#include "resource_entitlement/digest.hpp"
#include "resource_entitlement/text.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace re_test {
namespace {

std::atomic<std::uint64_t> g_sequence{0};

[[nodiscard]] std::string unique_suffix() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::uint64_t counter = g_sequence.fetch_add(1u, std::memory_order_relaxed);
#if defined(_WIN32)
  const auto pid = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  const auto pid = static_cast<std::uint64_t>(::getpid());
#endif
  return std::to_string(pid) + "-" + std::to_string(counter) + "-" + std::to_string(now);
}

#if defined(_WIN32)
[[nodiscard]] std::wstring to_wide(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

[[nodiscard]] std::string to_narrow(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                       nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  (void)WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr,
                            nullptr);
  return out;
}

[[nodiscard]] std::wstring extended_path(const std::filesystem::path& path) {
  std::wstring raw = path.wstring();
  if (raw.size() >= 4u && raw.compare(0u, 4u, L"\\\\?\\") == 0) {
    return raw;
  }
  if (raw.size() >= 2u && raw[0] == L'\\' && raw[1] == L'\\') {
    return L"\\\\?\\UNC\\" + raw.substr(2u);
  }
  return L"\\\\?\\" + raw;
}

[[nodiscard]] std::string quote_argument(const std::string& argument) {
  if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string::npos) {
    return argument;
  }
  std::string out = "\"";
  std::size_t index = 0;
  while (index < argument.size()) {
    std::size_t backslashes = 0;
    while (index < argument.size() && argument[index] == '\\') {
      ++index;
      ++backslashes;
    }
    if (index == argument.size()) {
      out.append(backslashes * 2u, '\\');
      break;
    }
    if (argument[index] == '"') {
      out.append((backslashes * 2u) + 1u, '\\');
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(argument[index]);
    }
    ++index;
  }
  out.push_back('"');
  return out;
}

[[nodiscard]] std::vector<wchar_t> build_environment_block(const std::vector<std::string>& extra) {
  std::vector<std::string> entries;
  LPWCH current = GetEnvironmentStringsW();
  if (current != nullptr) {
    const wchar_t* cursor = current;
    while (*cursor != L'\0') {
      const std::wstring entry(cursor);
      cursor += entry.size() + 1u;
      entries.push_back(to_narrow(entry));
    }
    FreeEnvironmentStringsW(current);
  }
  for (const std::string& addition : extra) {
    const std::size_t equals = addition.find('=');
    const std::string key = equals == std::string::npos ? addition : addition.substr(0, equals);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&key](const std::string& existing) {
                                   const std::size_t at = existing.find('=');
                                   return at != std::string::npos && existing.substr(0, at) == key;
                                 }),
                  entries.end());
    entries.push_back(addition);
  }
  std::vector<wchar_t> block;
  for (const std::string& entry : entries) {
    const std::wstring wide = to_wide(entry);
    block.insert(block.end(), wide.begin(), wide.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}
#endif

}  // namespace

std::filesystem::path system_temp_root() {
  std::error_code ec;
  auto path = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return std::filesystem::path(".");
  }
  return path;
}

void remove_tree_force(const std::filesystem::path& path) noexcept {
  std::error_code ec;
#if defined(_WIN32)
  // Very long paths exceed the legacy limit unless the extended prefix is used.
  const std::filesystem::path target(extended_path(path));
#else
  const std::filesystem::path target = path;
#endif
  if (!std::filesystem::exists(target, ec)) {
    return;
  }
  // Clear read-only attributes first: Windows refuses to delete them.
  std::filesystem::recursive_directory_iterator iterator(
      target, std::filesystem::directory_options::skip_permission_denied, ec);
  if (!ec) {
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
      std::error_code entry_ec;
      const auto status = iterator->symlink_status(entry_ec);
      if (!entry_ec && std::filesystem::is_regular_file(status)) {
#if defined(_WIN32)
        const std::wstring wide = extended_path(iterator->path());
        (void)SetFileAttributesW(wide.c_str(), FILE_ATTRIBUTE_NORMAL);
#else
        std::filesystem::permissions(iterator->path(), std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, entry_ec);
#endif
      }
      iterator.increment(ec);
      if (ec) {
        break;
      }
    }
  }
  std::filesystem::remove_all(target, ec);
}

TempDirectory::TempDirectory(std::string_view hint) {
  std::string leaf = "resource-entitlement-test-";
  leaf.append(hint.empty() ? std::string_view("case") : hint);
  leaf.push_back('-');
  leaf.append(unique_suffix());
  path_ = system_temp_root() / leaf;
  std::error_code ec;
  std::filesystem::create_directories(path_, ec);
  if (ec) {
    throw std::runtime_error("could not create temporary directory: " + path_.string());
  }
}

TempDirectory::~TempDirectory() { remove_tree_force(path_); }

std::filesystem::path TempDirectory::child(std::string_view name) const {
  return path_ / std::filesystem::path(std::string(name));
}

entl::Timestamp instant(std::int64_t unix_seconds) {
  const auto nanos = unix_seconds * 1000000000LL;
  auto parsed = entl::Timestamp::from_unix_nanos(nanos);
  if (!parsed.has_value()) {
    throw std::runtime_error("test fixture: invalid instant");
  }
  return parsed.value();
}

entl::Sha256Digest digest_of(std::string_view text) { return entl::Sha256::hash(text); }

entl::RequestId request_id(std::string_view seed) { return entl::RequestId::derive(seed); }

entl::EntitlementId entitlement_id(std::string_view hex) {
  auto parsed = entl::EntitlementId::from_hex(hex);
  if (!parsed.has_value()) {
    throw std::runtime_error("test fixture: invalid entitlement identity '" + std::string(hex) + "'");
  }
  return parsed.value();
}

entl::AuthorityUpdateRequest authority_request(std::string_view seed, entl::Timestamp now,
                                               entl::Revision expected,
                                               std::uint64_t capacity_generation) {
  entl::AuthorityUpdateRequest request;
  request.request_id = request_id(seed);
  request.now = now;
  request.expected_snapshot_revision = expected;
  request.facility_capacity_generation = entl::Generation::from_value(capacity_generation);
  request.publisher = must_parse<entl::ActorId>("facility-capacity");
  request.note = "published by Facility Capacity";
  return request;
}

entl::StoreOpenOptions test_options(const std::filesystem::path& directory, bool create_if_missing) {
  entl::StoreOpenOptions options;
  options.directory = directory;
  options.create_if_missing = create_if_missing;
  return options;
}

entl::Result<std::unique_ptr<entl::Store>> open_store_at(const std::filesystem::path& directory,
                                                          bool create_if_missing, bool fence_on_open,
                                                          std::uint32_t idempotency_capacity) {
  entl::StoreOpenOptions options = test_options(directory, create_if_missing);
  options.fence_live_authority_on_open = fence_on_open;
  options.max_idempotency_entries = idempotency_capacity;
  return entl::Store::open(options);
}

StoreFixture::StoreFixture() : StoreFixture(test_options(std::filesystem::path(), true)) {}

StoreFixture::StoreFixture(const entl::StoreOpenOptions& options)
    : directory_(options.directory.empty() ? std::string_view("store") : std::string_view("store")) {
  entl::StoreOpenOptions adjusted = options;
  adjusted.directory = directory_.path();
  auto opened = entl::Store::open(adjusted);
  if (!opened.has_value()) {
    throw std::runtime_error("StoreFixture: " + opened.error().to_string());
  }
  store_ = std::move(opened.value());
}

entl::Revision StoreFixture::publish_authority(std::uint64_t capacity_generation,
                                               std::uint64_t policy_revision,
                                               std::uint64_t envelope_revision,
                                               std::uint64_t service_class_revision) {
  counter_ += 1u;
  now_ = instant(1700000000 + static_cast<std::int64_t>(counter_));
  entl::AuthorityUpdateRequest request =
      authority_request("authority-" + std::to_string(counter_), now_, store_->authority().revision,
                        capacity_generation);
  request.facility_policy_revision = entl::Revision::from_value(policy_revision);
  request.resource_envelope_revision = entl::Revision::from_value(envelope_revision);
  request.service_class_revision = entl::Revision::from_value(service_class_revision);
  request.policy_context_digest = digest_of("policy-context-" + std::to_string(capacity_generation));
  request.envelope_binding_digest = digest_of("envelope-" + std::to_string(capacity_generation));
  auto outcome = store_->update_authority(request);
  if (!outcome.has_value()) {
    throw std::runtime_error("publish_authority: " + outcome.error().to_string());
  }
  return store_->authority().revision;
}

entl::GrantRequest StoreFixture::make_grant(std::string_view id_seed, std::uint64_t units,
                                            std::string_view tenant, std::string_view scope) const {
  entl::GrantRequest request;
  request.request_id = request_id(id_seed);
  request.now = now_;
  request.holder = must_parse<entl::TenantId>(tenant);
  request.service = must_parse<entl::ServiceId>("inference");
  request.service_class = must_parse<entl::ServiceClassId>("gold");
  request.scope.facility = must_parse<entl::FacilityId>("dc-1");
  request.scope.resource_type = must_parse<entl::ResourceTypeId>("accelerator");
  request.scope.scope = must_parse<entl::ResourceScopeId>(scope);
  request.unit = entl::Unit::kCount;
  auto quantity = entl::Quantity::make(entl::Unit::kCount, units);
  request.quantity = quantity.value();
  request.priority.klass = entl::PriorityClass::kStandard;
  request.priority.rank = 0;
  request.effective_from = instant(1699999000);
  request.expires_at = instant(1799999000);
  request.admission_decision_digest = digest_of(std::string("admission-") + std::string(id_seed));
  request.fence = entl::FenceMask::standard();
  request.actor = must_parse<entl::ActorId>("operator-1");
  return request;
}

entl::EntitlementId StoreFixture::grant_full(const entl::GrantRequest& request) {
  auto outcome = store_->grant(request);
  if (!outcome.has_value()) {
    throw std::runtime_error("grant_full: " + outcome.error().to_string());
  }
  return outcome.value().primary_id;
}

entl::EntitlementId StoreFixture::grant_default(std::string_view id_seed, std::uint64_t units,
                                                std::string_view tenant, std::string_view scope) {
  counter_ += 1u;
  now_ = instant(1700000000 + static_cast<std::int64_t>(counter_));
  entl::GrantRequest request = make_grant(id_seed, units, tenant, scope);
  request.now = now_;
  request.effective_from = instant(1699999000);
  request.expires_at = instant(1799999000);
  return grant_full(request);
}

std::filesystem::path executable_from_env(const char* name) {
#if defined(_WIN32)
  char* buffer = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
    return {};
  }
  const std::filesystem::path value(buffer);
  std::free(buffer);
  return value;
#else
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return {};
  }
  return std::filesystem::path(value);
#endif
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot read " + path.string());
  }
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot write " + path.string());
  }
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// ---------------------------------------------------------------------------
// Process handling
// ---------------------------------------------------------------------------

struct ChildProcess::State {
#if defined(_WIN32)
  HANDLE process{nullptr};
  HANDLE thread{nullptr};
  DWORD pid{0};
#else
  pid_t pid{0};
  bool reaped{false};
  int status{0};
#endif
  std::filesystem::path output_path;
  bool finished{false};
  ProcessResult result{};
};

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : state_(std::move(other.state_)) {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    state_ = std::move(other.state_);
  }
  return *this;
}

ChildProcess::~ChildProcess() {
  if (state_) {
    terminate_now();
#if defined(_WIN32)
    if (state_->thread != nullptr) {
      CloseHandle(state_->thread);
      state_->thread = nullptr;
    }
    if (state_->process != nullptr) {
      CloseHandle(state_->process);
      state_->process = nullptr;
    }
#endif
    if (!state_->output_path.empty()) {
      std::error_code ec;
      std::filesystem::remove(state_->output_path, ec);
    }
  }
}

std::optional<ChildProcess> ChildProcess::start(const std::filesystem::path& executable,
                                                const std::vector<std::string>& arguments,
                                                const std::vector<std::string>& environment) {
  if (!std::filesystem::exists(executable)) {
    return std::nullopt;
  }
  ChildProcess child;
  child.state_ = std::make_shared<State>();
  child.state_->output_path =
      system_temp_root() / ("resource-entitlement-child-" + unique_suffix() + ".log");

#if defined(_WIN32)
  std::string command = quote_argument(executable.string());
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command.append(quote_argument(argument));
  }
  // A null environment block means "inherit the parent's environment", which is
  // both simpler and less error-prone than rebuilding it when nothing extra is
  // requested.
  std::vector<wchar_t> environment_block;
  if (!environment.empty()) {
    environment_block = build_environment_block(environment);
  }
  std::wstring mutable_command = to_wide(command);
  mutable_command.push_back(L'\0');
  std::wstring mutable_output = to_wide(child.state_->output_path.string());

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE output = CreateFileW(mutable_output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    std::fprintf(stderr, "ChildProcess::start: cannot create %s (win32 error %lu)\n",
                 child.state_->output_path.string().c_str(),
                 static_cast<unsigned long>(GetLastError()));
    return std::nullopt;
  }
  // A child that inherits standard handles needs a valid handle for each of
  // them; when the parent has no console (as under a test runner) the inherited
  // standard input handle is invalid and CreateProcess rejects the request.
  HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  bool owned_input = false;
  if (input == nullptr || input == INVALID_HANDLE_VALUE) {
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    owned_input = true;
  }
  if (input == INVALID_HANDLE_VALUE) {
    CloseHandle(output);
    std::fprintf(stderr, "ChildProcess::start: no usable standard input handle\n");
    return std::nullopt;
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = input;
  PROCESS_INFORMATION info{};
  LPVOID environment_pointer = environment_block.empty()
                                   ? nullptr
                                   : static_cast<LPVOID>(environment_block.data());
  const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, environment_pointer, nullptr, &startup, &info);
  const DWORD create_error = (created == 0) ? GetLastError() : 0u;
  CloseHandle(output);
  if (owned_input) {
    CloseHandle(input);
  }
  if (created == 0) {
    std::fprintf(stderr, "ChildProcess::start: CreateProcess failed with win32 error %lu for %s\n",
                 static_cast<unsigned long>(create_error), command.c_str());
    return std::nullopt;
  }
  child.state_->process = info.hProcess;
  child.state_->thread = info.hThread;
  child.state_->pid = info.dwProcessId;
#else
  std::vector<std::string> argv_storage;
  argv_storage.push_back(executable.string());
  for (const std::string& argument : arguments) {
    argv_storage.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(argv_storage.size() + 1u);
  for (std::string& value : argv_storage) {
    argv.push_back(value.data());
  }
  argv.push_back(nullptr);
  std::vector<std::string> env_storage;
  std::vector<char*> envp;
  if (!environment.empty()) {
    for (char** cursor = environ; cursor != nullptr && *cursor != nullptr; ++cursor) {
      env_storage.push_back(*cursor);
    }
    for (const std::string& addition : environment) {
      env_storage.push_back(addition);
    }
    for (std::string& value : env_storage) {
      envp.push_back(value.data());
    }
    envp.push_back(nullptr);
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 1, child.state_->output_path.string().c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&actions, 1, 2);
  pid_t pid = 0;
  const int status = posix_spawn(&pid, executable.string().c_str(), &actions, nullptr, argv.data(),
                                 environment.empty() ? environ : envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (status != 0) {
    return std::nullopt;
  }
  child.state_->pid = pid;
#endif
  return std::optional<ChildProcess>(std::move(child));
}

bool ChildProcess::running() const noexcept {
  if (!state_ || state_->finished) {
    return false;
  }
#if defined(_WIN32)
  return WaitForSingleObject(state_->process, 0) == WAIT_TIMEOUT;
#else
  int status = 0;
  const pid_t result = ::waitpid(state_->pid, &status, WNOHANG);
  if (result == state_->pid) {
    state_->status = status;
    state_->reaped = true;
    return false;
  }
  return true;
#endif
}

std::uint64_t ChildProcess::pid() const noexcept {
  if (!state_) {
    return 0;
  }
#if defined(_WIN32)
  return static_cast<std::uint64_t>(state_->pid);
#else
  return static_cast<std::uint64_t>(state_->pid);
#endif
}

void ChildProcess::terminate_now() {
  if (!state_ || state_->finished) {
    return;
  }
#if defined(_WIN32)
  if (state_->process != nullptr) {
    (void)TerminateProcess(state_->process, 137);
  }
#else
  (void)::kill(state_->pid, SIGKILL);
#endif
}

ProcessResult ChildProcess::wait() {
  if (!state_) {
    return ProcessResult{};
  }
  if (state_->finished) {
    return state_->result;
  }
  int exit_code = -1;
#if defined(_WIN32)
  (void)WaitForSingleObject(state_->process, INFINITE);
  DWORD code = 0;
  if (GetExitCodeProcess(state_->process, &code) != 0) {
    exit_code = static_cast<int>(code);
  }
  if (state_->thread != nullptr) {
    CloseHandle(state_->thread);
    state_->thread = nullptr;
  }
  if (state_->process != nullptr) {
    CloseHandle(state_->process);
    state_->process = nullptr;
  }
#else
  if (!state_->reaped) {
    int status = 0;
    (void)::waitpid(state_->pid, &status, 0);
    state_->status = status;
    state_->reaped = true;
  }
  if (WIFEXITED(state_->status)) {
    exit_code = WEXITSTATUS(state_->status);
  } else if (WIFSIGNALED(state_->status)) {
    exit_code = 128 + WTERMSIG(state_->status);
  }
#endif
  ProcessResult result;
  result.started = true;
  result.exit_code = exit_code;
  try {
    const std::vector<std::uint8_t> bytes = read_bytes(state_->output_path);
    result.output.assign(bytes.begin(), bytes.end());
  } catch (const std::exception&) {
    result.output.clear();
  }
  {
    std::error_code ec;
    std::filesystem::remove(state_->output_path, ec);
  }
  state_->finished = true;
  state_->result = result;
  return result;
}

ProcessResult run_process(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                          const std::vector<std::string>& environment) {
  auto child = ChildProcess::start(executable, arguments, environment);
  if (!child.has_value()) {
    ProcessResult result;
    result.started = false;
    result.error = "could not start " + executable.string();
    return result;
  }
  return child->wait();
}

}  // namespace re_test
