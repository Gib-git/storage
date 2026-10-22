#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace storage {

enum class NodeKind : uint8_t { File = 0, Directory = 1, Symlink = 2, Other = 3 };

namespace NodeFlag {
inline constexpr uint8_t Unreadable = 1 << 0;      // directory could not be listed
inline constexpr uint8_t HardlinkRepeat = 1 << 1;  // extra link to an inode counted elsewhere
inline constexpr uint8_t Excluded = 1 << 2;        // skipped by exclude rules / mount boundary
}  // namespace NodeFlag

// One entry of a scanned tree. Directories carry aggregated totals of everything
// beneath them once the tree has been finalized.
struct Node {
    std::string name;  // UTF-8; the root node holds the full root path
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;

    uint64_t size = 0;       // apparent bytes (aggregated for directories)
    uint64_t files = 0;      // regular files at or beneath this node
    uint64_t dirs = 0;       // directories beneath this node (excluding itself)
    uint64_t errors = 0;     // unreadable directories at or beneath this node
    int64_t mtime = 0;       // seconds since epoch
    uint64_t dev = 0;
    uint64_t ino = 0;
    uint32_t nlink = 1;
    NodeKind kind = NodeKind::File;
    uint8_t flags = 0;

    bool isDir() const { return kind == NodeKind::Directory; }

    // Full UTF-8 path, joined with `sep`.
    std::string path(char sep = '/') const;
};

struct ScanTree {
    std::unique_ptr<Node> root;
    std::string volume = "local";  // "local" or a NAS connection name
    int64_t started = 0;           // seconds since epoch
    int64_t finished = 0;
    bool complete = false;         // false when cancelled
    int64_t id = 0;                // index row id once persisted

    // Recompute directory totals bottom-up and sort children by size (desc).
    void finalize();

    // Find the node for a path under the root, or nullptr.
    Node* find(const std::string& path) const;
};

}  // namespace storage
