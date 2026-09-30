// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "fileio.hpp"

#include <atomic>
#include <cstring>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace entl::detail {
namespace {

std::atomic<std::uint64_t> g_temporary_counter{0};

#if defined(_WIN32)

[[nodiscard]] std::wstring to_extended_path(const std::filesystem::path& path) {
  std::wstring raw = path.wstring();
  if (raw.size() >= 4u && raw.compare(0u, 4u, L"\\\\?\\") == 0) {
    return raw;
  }
  std::error_code ec;
  std::filesystem::path absolute = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    absolute = std::filesystem::absolute(path, ec);
  }
  if (ec) {
    absolute = path;
  }
  std::wstring wide = absolute.wstring();
  if (wide.size() >= 4u && wide.compare(0u, 4u, L"\\\\?\\") == 0) {
    return wide;
  }
  if (wide.size() >= 2u && wide[0] == L'\\' && wide[1] == L'\\') {
    return L"\\\\?\\UNC\\" + wide.substr(2u);
  }
  return L"\\\\?\\" + wide;
}

[[nodiscard]] Error win32_error(const char* operation, const std::filesystem::path& path, DWORD code) {
  return Error(ErrorCode::kIoError, std::string(operation) + " failed",
               "win32=" + std::to_string(code) + " path=" + path.filename().string());
}

#else

[[nodiscard]] Error posix_error(const char* operation, const std::filesystem::path& path, int code) {
  return Error(ErrorCode::kIoError, std::string(operation) + " failed",
               "errno=" + std::to_string(code) + " path=" + path.filename().string());
}

#endif

}  // namespace

#if defined(_WIN32)
/// Route directory operations through the extended namespace so a store path
/// longer than MAX_PATH works, and so a component that happens to be a legacy
/// device name is treated as an ordinary name rather than silently redirected.
[[nodiscard]] std::filesystem::path long_path(const std::filesystem::path& path) {
  return std::filesystem::path(to_extended_path(path));
}
#else
[[nodiscard]] std::filesystem::path long_path(const std::filesystem::path& path) { return path; }
#endif

bool path_exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(long_path(path), ec) && !ec;
}

Result<void> ensure_directory(const std::filesystem::path& path) {
  std::error_code ec;
  const std::filesystem::path target = long_path(path);
  if (std::filesystem::exists(target, ec)) {
    if (ec) {
      return Error(ErrorCode::kPathError, "cannot query store directory", path.string());
    }
    if (!std::filesystem::is_directory(target, ec) || ec) {
      return Error(ErrorCode::kNotADirectory, "store path exists but is not a directory", path.string());
    }
    return {};
  }
  std::filesystem::create_directories(target, ec);
  if (ec) {
    return Error(ErrorCode::kPathError, "cannot create store directory", ec.message());
  }
  if (!std::filesystem::is_directory(target, ec) || ec) {
    return Error(ErrorCode::kNotADirectory, "store path is not a directory after creation", path.string());
  }
  return {};
}

Result<std::vector<std::filesystem::path>> list_directory(const std::filesystem::path& path) {
  std::error_code ec;
  std::vector<std::filesystem::path> entries;
  std::filesystem::directory_iterator iterator(long_path(path), ec);
  if (ec) {
    return Error(ErrorCode::kPathError, "cannot enumerate store directory", ec.message());
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    entries.push_back(iterator->path().filename());
    iterator.increment(ec);
    if (ec) {
      return Error(ErrorCode::kPathError, "cannot enumerate store directory", ec.message());
    }
  }
  return entries;
}

Result<std::uint64_t> file_size_bytes(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::wstring wide = to_extended_path(path);
  HANDLE handle = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return win32_error("open for size", path, GetLastError());
  }
  LARGE_INTEGER size{};
  const BOOL ok = GetFileSizeEx(handle, &size);
  const DWORD code = ok ? 0u : GetLastError();
  CloseHandle(handle);
  if (ok == 0) {
    return win32_error("query size", path, code);
  }
  return static_cast<std::uint64_t>(size.QuadPart);
#else
  struct stat info {};
  if (::stat(path.string().c_str(), &info) != 0) {
    return posix_error("stat", path, errno);
  }
  return static_cast<std::uint64_t>(info.st_size);
#endif
}

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes) {
#if defined(_WIN32)
  const std::wstring wide = to_extended_path(path);
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return win32_error("open", path, GetLastError());
  }
  LARGE_INTEGER size{};
  if (GetFileSizeEx(handle, &size) == 0) {
    const DWORD code = GetLastError();
    CloseHandle(handle);
    return win32_error("query size", path, code);
  }
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
    CloseHandle(handle);
    return Error(ErrorCode::kLimitExceeded, "file exceeds the configured read bound", path.filename().string());
  }
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (buffer.size() - offset) > 0x10000000u ? 0x10000000u : (buffer.size() - offset));
    DWORD read = 0;
    if (ReadFile(handle, buffer.data() + offset, chunk, &read, nullptr) == 0) {
      const DWORD code = GetLastError();
      CloseHandle(handle);
      return win32_error("read", path, code);
    }
    if (read == 0) {
      break;
    }
    offset += read;
  }
  CloseHandle(handle);
  buffer.resize(offset);
  return buffer;
#else
  const int descriptor = ::open(path.string().c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return posix_error("open", path, errno);
  }
  struct stat info {};
  if (::fstat(descriptor, &info) != 0) {
    const int code = errno;
    ::close(descriptor);
    return posix_error("fstat", path, code);
  }
  if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > max_bytes) {
    ::close(descriptor);
    return Error(ErrorCode::kLimitExceeded, "file exceeds the configured read bound", path.filename().string());
  }
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(info.st_size));
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const ssize_t read = ::read(descriptor, buffer.data() + offset, buffer.size() - offset);
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int code = errno;
      ::close(descriptor);
      return posix_error("read", path, code);
    }
    if (read == 0) {
      break;
    }
    offset += static_cast<std::size_t>(read);
  }
  ::close(descriptor);
  buffer.resize(offset);
  return buffer;
#endif
}

Result<void> write_file_atomic(const std::filesystem::path& target, std::span<const std::uint8_t> data,
                               std::uint64_t sequence_hint) {
  const std::uint64_t counter = g_temporary_counter.fetch_add(1u, std::memory_order_relaxed);
  std::filesystem::path temporary = target;
  temporary += ".tmp-" + std::to_string(sequence_hint) + "-" + std::to_string(counter);

#if defined(_WIN32)
  const std::wstring wide = to_extended_path(temporary);
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return win32_error("create temporary", temporary, GetLastError());
  }
  std::size_t offset = 0;
  bool failed = false;
  Error failure{};
  while (offset < data.size()) {
    const DWORD chunk =
        static_cast<DWORD>((data.size() - offset) > 0x10000000u ? 0x10000000u : (data.size() - offset));
    DWORD written = 0;
    if (WriteFile(handle, data.data() + offset, chunk, &written, nullptr) == 0) {
      failure = win32_error("write temporary", temporary, GetLastError());
      failed = true;
      break;
    }
    offset += written;
  }
  if (!failed && FlushFileBuffers(handle) == 0) {
    failure = win32_error("flush temporary", temporary, GetLastError());
    failed = true;
  }
  CloseHandle(handle);
  if (failed) {
    (void)remove_file_if_exists(temporary);
    return failure;
  }
  if (MoveFileExW(to_extended_path(temporary).c_str(), to_extended_path(target).c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    const DWORD code = GetLastError();
    (void)remove_file_if_exists(temporary);
    return win32_error("publish temporary", target, code);
  }
  return {};
#else
  const int descriptor = ::open(temporary.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    return posix_error("create temporary", temporary, errno);
  }
  std::size_t offset = 0;
  bool failed = false;
  Error failure{};
  while (offset < data.size()) {
    const ssize_t written = ::write(descriptor, data.data() + offset, data.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      failure = posix_error("write temporary", temporary, errno);
      failed = true;
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  if (!failed && ::fsync(descriptor) != 0) {
    failure = posix_error("flush temporary", temporary, errno);
    failed = true;
  }
  if (::close(descriptor) != 0 && !failed) {
    failure = posix_error("close temporary", temporary, errno);
    failed = true;
  }
  if (failed) {
    (void)remove_file_if_exists(temporary);
    return failure;
  }
  if (::rename(temporary.string().c_str(), target.string().c_str()) != 0) {
    const int code = errno;
    (void)remove_file_if_exists(temporary);
    return posix_error("publish temporary", target, code);
  }
  return flush_directory(target.parent_path());
#endif
}

Result<void> remove_file(const std::filesystem::path& path) {
#if defined(_WIN32)
  if (DeleteFileW(to_extended_path(path).c_str()) == 0) {
    return win32_error("delete", path, GetLastError());
  }
  return {};
#else
  if (::unlink(path.string().c_str()) != 0) {
    return posix_error("unlink", path, errno);
  }
  return {};
#endif
}

Result<void> remove_file_if_exists(const std::filesystem::path& path) {
  if (!path_exists(path)) {
    return {};
  }
  return remove_file(path);
}

Result<void> flush_directory(const std::filesystem::path& path) {
#if defined(_WIN32)
  (void)path;
  return {};
#else
  const int descriptor = ::open(path.string().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    return posix_error("open directory", path, errno);
  }
  const int status = ::fsync(descriptor);
  const int code = errno;
  ::close(descriptor);
  if (status != 0) {
    return posix_error("flush directory", path, code);
  }
  return {};
#endif
}

// ---------------------------------------------------------------------------
// RandomAccessFile
// ---------------------------------------------------------------------------

struct RandomAccessFile::State {
#if defined(_WIN32)
  HANDLE handle{INVALID_HANDLE_VALUE};
  ~State() {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
#else
  int descriptor{-1};
  ~State() {
    if (descriptor >= 0) {
      ::close(descriptor);
    }
  }
#endif
  State() = default;
  State(const State&) = delete;
  State& operator=(const State&) = delete;
};

RandomAccessFile::RandomAccessFile() = default;

RandomAccessFile::~RandomAccessFile() = default;

RandomAccessFile::RandomAccessFile(RandomAccessFile&& other) noexcept : state_(std::move(other.state_)) {}

RandomAccessFile& RandomAccessFile::operator=(RandomAccessFile&& other) noexcept {
  if (this != &other) {
    state_ = std::move(other.state_);
  }
  return *this;
}

bool RandomAccessFile::is_open() const noexcept { return state_ != nullptr; }

Result<RandomAccessFile> RandomAccessFile::open(const std::filesystem::path& path, bool create) {
#if defined(_WIN32)
  const std::wstring wide = to_extended_path(path);
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return win32_error("open", path, GetLastError());
  }
  RandomAccessFile file;
  file.state_ = std::make_unique<State>();
  file.state_->handle = handle;
  return file;
#else
  const int flags = create ? (O_RDWR | O_CREAT | O_CLOEXEC) : (O_RDWR | O_CLOEXEC);
  const int descriptor = ::open(path.string().c_str(), flags, 0644);
  if (descriptor < 0) {
    return posix_error("open", path, errno);
  }
  RandomAccessFile file;
  file.state_ = std::make_unique<State>();
  file.state_->descriptor = descriptor;
  return file;
#endif
}

Result<std::uint64_t> RandomAccessFile::size() const {
  if (!state_) {
    return Error(ErrorCode::kIoError, "file is not open");
  }
#if defined(_WIN32)
  LARGE_INTEGER value{};
  if (GetFileSizeEx(state_->handle, &value) == 0) {
    return Error(ErrorCode::kIoError, "query size failed", std::to_string(GetLastError()));
  }
  return static_cast<std::uint64_t>(value.QuadPart);
#else
  struct stat info {};
  if (::fstat(state_->descriptor, &info) != 0) {
    return posix_error("fstat", {}, errno);
  }
  return static_cast<std::uint64_t>(info.st_size);
#endif
}

Result<void> RandomAccessFile::write_at(std::uint64_t offset, std::span<const std::uint8_t> data) {
  if (!state_) {
    return Error(ErrorCode::kIoError, "file is not open");
  }
#if defined(_WIN32)
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (SetFilePointerEx(state_->handle, position, nullptr, FILE_BEGIN) == 0) {
    return Error(ErrorCode::kIoError, "seek for write failed", std::to_string(GetLastError()));
  }
  std::size_t written_total = 0;
  while (written_total < data.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (data.size() - written_total) > 0x10000000u ? 0x10000000u : (data.size() - written_total));
    DWORD written = 0;
    if (WriteFile(state_->handle, data.data() + written_total, chunk, &written, nullptr) == 0) {
      return Error(ErrorCode::kIoError, "write failed", std::to_string(GetLastError()));
    }
    written_total += written;
  }
  return {};
#else
  std::size_t written_total = 0;
  while (written_total < data.size()) {
    const ssize_t written =
        ::pwrite(state_->descriptor, data.data() + written_total, data.size() - written_total,
                 static_cast<off_t>(offset + written_total));
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return posix_error("pwrite", {}, errno);
    }
    written_total += static_cast<std::size_t>(written);
  }
  return {};
#endif
}

Result<std::vector<std::uint8_t>> RandomAccessFile::read_at(std::uint64_t offset, std::size_t length) const {
  if (!state_) {
    return Error(ErrorCode::kIoError, "file is not open");
  }
  std::vector<std::uint8_t> buffer(length);
#if defined(_WIN32)
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (SetFilePointerEx(state_->handle, position, nullptr, FILE_BEGIN) == 0) {
    return Error(ErrorCode::kIoError, "seek for read failed", std::to_string(GetLastError()));
  }
  std::size_t read_total = 0;
  while (read_total < length) {
    const DWORD chunk = static_cast<DWORD>(
        (length - read_total) > 0x10000000u ? 0x10000000u : (length - read_total));
    DWORD read = 0;
    if (ReadFile(state_->handle, buffer.data() + read_total, chunk, &read, nullptr) == 0) {
      return Error(ErrorCode::kIoError, "read failed", std::to_string(GetLastError()));
    }
    if (read == 0) {
      break;
    }
    read_total += read;
  }
  buffer.resize(read_total);
  return buffer;
#else
  std::size_t read_total = 0;
  while (read_total < length) {
    const ssize_t read = ::pread(state_->descriptor, buffer.data() + read_total, length - read_total,
                                 static_cast<off_t>(offset + read_total));
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      return posix_error("pread", {}, errno);
    }
    if (read == 0) {
      break;
    }
    read_total += static_cast<std::size_t>(read);
  }
  buffer.resize(read_total);
  return buffer;
#endif
}

Result<void> RandomAccessFile::truncate(std::uint64_t length) {
  if (!state_) {
    return Error(ErrorCode::kIoError, "file is not open");
  }
#if defined(_WIN32)
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(length);
  if (SetFilePointerEx(state_->handle, position, nullptr, FILE_BEGIN) == 0) {
    return Error(ErrorCode::kIoError, "seek for truncate failed", std::to_string(GetLastError()));
  }
  if (SetEndOfFile(state_->handle) == 0) {
    return Error(ErrorCode::kIoError, "truncate failed", std::to_string(GetLastError()));
  }
  return {};
#else
  if (::ftruncate(state_->descriptor, static_cast<off_t>(length)) != 0) {
    return posix_error("ftruncate", {}, errno);
  }
  return {};
#endif
}

Result<void> RandomAccessFile::flush() {
  if (!state_) {
    return Error(ErrorCode::kIoError, "file is not open");
  }
#if defined(_WIN32)
  if (FlushFileBuffers(state_->handle) == 0) {
    return Error(ErrorCode::kIoError, "flush failed", std::to_string(GetLastError()));
  }
  return {};
#else
  if (::fdatasync(state_->descriptor) != 0 && errno != EINVAL) {
    return posix_error("fdatasync", {}, errno);
  }
  return {};
#endif
}

// ---------------------------------------------------------------------------
// WriterLock
// ---------------------------------------------------------------------------

struct WriterLock::State {
#if defined(_WIN32)
  HANDLE handle{INVALID_HANDLE_VALUE};
  ~State() {
    if (handle != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlapped{};
      (void)UnlockFileEx(handle, 0, 1, 0, &overlapped);
      CloseHandle(handle);
    }
  }
#else
  int descriptor{-1};
  ~State() {
    if (descriptor >= 0) {
      (void)::flock(descriptor, LOCK_UN);
      ::close(descriptor);
    }
  }
#endif
  State() = default;
  State(const State&) = delete;
  State& operator=(const State&) = delete;
};

WriterLock::WriterLock() = default;

WriterLock::~WriterLock() = default;

WriterLock::WriterLock(WriterLock&& other) noexcept : state_(std::move(other.state_)) {}

WriterLock& WriterLock::operator=(WriterLock&& other) noexcept {
  if (this != &other) {
    state_ = std::move(other.state_);
  }
  return *this;
}

bool WriterLock::held() const noexcept { return state_ != nullptr; }

Result<WriterLock> WriterLock::acquire(const std::filesystem::path& lock_path) {
#if defined(_WIN32)
  const std::wstring wide = to_extended_path(lock_path);
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return win32_error("open lock file", lock_path, GetLastError());
  }
  OVERLAPPED overlapped{};
  if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) == 0) {
    const DWORD code = GetLastError();
    CloseHandle(handle);
    if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) {
      return Error(ErrorCode::kWriterLockHeld,
                   "another process holds the writer lock for this store directory");
    }
    return Error(ErrorCode::kLockError, "cannot acquire the writer lock", std::to_string(code));
  }
  WriterLock lock;
  lock.state_ = std::make_unique<State>();
  lock.state_->handle = handle;
  return lock;
#else
  const int descriptor = ::open(lock_path.string().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    return posix_error("open lock file", lock_path, errno);
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int code = errno;
    ::close(descriptor);
    if (code == EWOULDBLOCK || code == EAGAIN) {
      return Error(ErrorCode::kWriterLockHeld,
                   "another process holds the writer lock for this store directory");
    }
    return Error(ErrorCode::kLockError, "cannot acquire the writer lock", std::to_string(code));
  }
  WriterLock lock;
  lock.state_ = std::make_unique<State>();
  lock.state_->descriptor = descriptor;
  return lock;
#endif
}

}  // namespace entl::detail
