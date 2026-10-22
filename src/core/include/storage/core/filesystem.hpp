#pragma once

#include <cstdint>
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
};

class LocalFileSystem final : public FileSystem {
public:
    std::error_code stat(const std::string& path, DirEntry& out) override;
    std::error_code list(const std::string& dir, std::vector<DirEntry>& out) override;
    std::string join(const std::string& dir, const std::string& name) const override;
    char separator() const override;
    std::string volumeName() const override { return "local"; }
};

}  // namespace storage
