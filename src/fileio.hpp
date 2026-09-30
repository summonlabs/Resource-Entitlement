// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal platform file primitives used by the durable store. Not installed.

#ifndef RESOURCE_ENTITLEMENT_SRC_FILEIO_HPP
#define RESOURCE_ENTITLEMENT_SRC_FILEIO_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "resource_entitlement/error.hpp"

namespace entl::detail {

/// Largest file this library will ever read into memory in one call.
inline constexpr std::uint64_t kMaxReadableFileBytes = 1ull << 31;  // 2 GiB

[[nodiscard]] bool path_exists(const std::filesystem::path& path);

[[nodiscard]] Result<void> ensure_directory(const std::filesystem::path& path);

[[nodiscard]] Result<std::vector<std::filesystem::path>> list_directory(const std::filesystem::path& path);

[[nodiscard]] Result<std::uint64_t> file_size_bytes(const std::filesystem::path& path);

/// Reads an entire regular file. Refuses directories, symlinks are followed,
/// and rejects files larger than \p max_bytes.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                                          std::uint64_t max_bytes);

/// Writes \p data to a temporary sibling, flushes it to stable storage, and
/// atomically replaces \p target. The temporary is removed on any failure.
[[nodiscard]] Result<void> write_file_atomic(const std::filesystem::path& target,
                                             std::span<const std::uint8_t> data,
                                             std::uint64_t sequence_hint);

[[nodiscard]] Result<void> remove_file(const std::filesystem::path& path);

[[nodiscard]] Result<void> remove_file_if_exists(const std::filesystem::path& path);

/// A randomly-accessible file handle used for the append-only journal and the
/// fixed-size manifest slots. One handle serves both reads and writes.
class RandomAccessFile {
public:
  RandomAccessFile();
  ~RandomAccessFile();
  RandomAccessFile(RandomAccessFile&& other) noexcept;
  RandomAccessFile& operator=(RandomAccessFile&& other) noexcept;
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;

  [[nodiscard]] static Result<RandomAccessFile> open(const std::filesystem::path& path, bool create);

  [[nodiscard]] Result<std::uint64_t> size() const;
  [[nodiscard]] Result<void> write_at(std::uint64_t offset, std::span<const std::uint8_t> data);
  [[nodiscard]] Result<std::vector<std::uint8_t>> read_at(std::uint64_t offset, std::size_t length) const;
  [[nodiscard]] Result<void> truncate(std::uint64_t length);
  [[nodiscard]] Result<void> flush();
  [[nodiscard]] bool is_open() const noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

/// An operating-system advisory exclusive lock, released by the kernel when the
/// owning process dies. The lock is taken without waiting.
class WriterLock {
public:
  WriterLock();
  ~WriterLock();
  WriterLock(WriterLock&& other) noexcept;
  WriterLock& operator=(WriterLock&& other) noexcept;
  WriterLock(const WriterLock&) = delete;
  WriterLock& operator=(const WriterLock&) = delete;

  /// Returns kWriterLockHeld when another process already holds the lock.
  [[nodiscard]] static Result<WriterLock> acquire(const std::filesystem::path& lock_path);

  [[nodiscard]] bool held() const noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

/// Flushes the process-wide directory metadata of \p path to stable storage
/// where the platform supports it; a no-op elsewhere.
[[nodiscard]] Result<void> flush_directory(const std::filesystem::path& path);

}  // namespace entl::detail

#endif  // RESOURCE_ENTITLEMENT_SRC_FILEIO_HPP
