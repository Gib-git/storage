#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "storage/core/node.hpp"

namespace storage {

struct DirEntry {
    std::string name;  // UTF-8
    NodeKind kind = NodeKind::File;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint64_t dev = 0;
    uint64_t ino = 0;
    uint32_t nlink = 1;
};

// Abstract filesystem so every feature works the same on local disks and on a
// NAS reached over SSH. Paths are UTF-8 strings in the filesystem's own syntax.
class FileSystem {
public:
    virtual ~FileSystem() = default;

    // Stat a single path without following a final symlink.
    virtual std::error_code stat(const std::string& path, DirEntry& out) = 0;

    // List the immediate children of `dir` (no "." or ".."), without following symlinks.
    virtual std::error_code list(const std::string& dir, std::vector<DirEntry>& out) = 0;

    virtual std::string join(const std::string& dir, const std::string& name) const = 0;
    virtual char separator() const = 0;
    virtual std::string volumeName() const = 0;

    // Optional fast path that walks a whole tree in one go (used for remote filesystems,
    // where a round trip per directory would be far too slow). nullptr = not supported.
    virtual std::unique_ptr<ScanTree> walkTree(const std::string& /*root*/, const std::atomic<bool>& /*cancel*/,
                                               std::atomic<uint64_t>& /*files*/, std::atomic<uint64_t>& /*dirs*/,
                                               std::atomic<uint64_t>& /*bytes*/) {
        return nullptr;
    }

    // Content hashes for duplicate detection, as lowercase hex SHA-256.
    // Partial: files up to 2 * kPartialChunk bytes are hashed whole; larger files
    // hash their first and last kPartialChunk bytes (reproducible remotely with
    // `{ head -c N f; tail -c N f; } | sha256sum`).
    static constexpr uint64_t kPartialChunk = 64 * 1024;
    virtual std::error_code hashPartial(const std::string& path, uint64_t size, std::string& out) = 0;
    // `bytesRead` (optional) is advanced as data is consumed, for progress reporting.
    virtual std::error_code hashFull(const std::string& path, std::string& out, const std::atomic<bool>* cancel,
                                     std::atomic<uint64_t>* bytesRead) = 0;
    // Byte-for-byte comparison of two files.
    virtual std::error_code sameContent(const std::string& a, const std::string& b, bool& equal,
                                        const std::atomic<bool>* cancel) = 0;

    // --- Mutations --------------------------------------------------------------
    // Each call either fully happens or leaves the original untouched; temporary
    // files use kTempSuffix so crash recovery can find and remove them.
    static constexpr const char* kTempSuffix = ".storage-tmp";

    // Recoverable delete: OS trash locally, a quarantine folder on a NAS.
    // `location` receives where the item went when that is known (enables undo).
    virtual std::error_code trash(const std::string& path, std::string& location) = 0;
    // Put a trashed item back. Fails if `original` exists again.
    virtual std::error_code restore(const std::string& location, const std::string& original) = 0;
    virtual std::error_code removeFile(const std::string& path) = 0;

    // Atomically replace `target` with a hard link to / copy-on-write clone of /
    // independent copy of `source`. Content must already be known to be identical.
    virtual std::error_code replaceWithHardlink(const std::string& target, const std::string& source) = 0;
    virtual std::error_code replaceWithClone(const std::string& target, const std::string& source) = 0;
    virtual std::error_code replaceWithCopy(const std::string& target, const std::string& source) = 0;

    // Building blocks for moves. renameNoReplace returns errc::file_exists rather
    // than overwrite, and errc::cross_device_link when a copy is needed instead.
    virtual std::error_code makeDirs(const std::string& dir) = 0;
    virtual std::error_code renameNoReplace(const std::string& from, const std::string& to) = 0;
    // Copy to a new file (must not exist), flushed to disk, preserving mtime.
    virtual std::error_code copyNew(const std::string& from, const std::string& to, const std::atomic<bool>* cancel) = 0;
    std::string parentOf(const std::string& path) const;

    // Read up to `max` bytes from the start of a file (metadata sniffing).
    virtual std::error_code readHead(const std::string& path, size_t max, std::string& out) = 0;
    // Remove an empty directory (fails if it is not empty).
    virtual std::error_code removeDir(const std::string& dir) = 0;
    // Remove a directory and everything in it, never following symlinks.
    virtual std::error_code removeTree(const std::string& dir) = 0;
};

class LocalFileSystem final : public FileSystem {
public:
    std::error_code stat(const std::string& path, DirEntry& out) override;
    std::error_code list(const std::string& dir, std::vector<DirEntry>& out) override;
    std::string join(const std::string& dir, const std::string& name) const override;
    char separator() const override;
    std::string volumeName() const override { return "local"; }
    std::error_code hashPartial(const std::string& path, uint64_t size, std::string& out) override;
    std::error_code hashFull(const std::string& path, std::string& out, const std::atomic<bool>* cancel,
                             std::atomic<uint64_t>* bytesRead) override;
    std::error_code sameContent(const std::string& a, const std::string& b, bool& equal,
                                const std::atomic<bool>* cancel) override;
    std::error_code trash(const std::string& path, std::string& location) override;
    std::error_code restore(const std::string& location, const std::string& original) override;
    std::error_code removeFile(const std::string& path) override;
    std::error_code replaceWithHardlink(const std::string& target, const std::string& source) override;
    std::error_code replaceWithClone(const std::string& target, const std::string& source) override;
    std::error_code replaceWithCopy(const std::string& target, const std::string& source) override;
    std::error_code makeDirs(const std::string& dir) override;
    std::error_code renameNoReplace(const std::string& from, const std::string& to) override;
    std::error_code copyNew(const std::string& from, const std::string& to, const std::atomic<bool>* cancel) override;
    std::error_code readHead(const std::string& path, size_t max, std::string& out) override;
    std::error_code removeDir(const std::string& dir) override;
    std::error_code removeTree(const std::string& dir) override;
};

// Platform helpers shared by LocalFileSystem and the organizer.
namespace local {
// Rename that fails with file_exists instead of overwriting.
std::error_code renameNoReplace(const std::string& from, const std::string& to);
// Rename that atomically replaces `to`.
std::error_code renameReplace(const std::string& from, const std::string& to);
// Copy `from` to the new file `to` (must not exist), preserving mode and mtime, flushed to disk.
std::error_code copyFile(const std::string& from, const std::string& to, const std::atomic<bool>* cancel = nullptr);
// Flush a directory entry change (rename/create) to disk where the OS supports it.
void syncParentDir(const std::string& path);
// Move to the OS trash. Implemented per platform.
std::error_code moveToTrash(const std::string& path, std::string& location);
}  // namespace local

}  // namespace storage
