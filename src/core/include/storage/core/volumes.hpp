#pragma once

#include <string>

#include "storage/core/filesystem.hpp"

namespace storage {

// Shared filesystem instances by volume name: "local" for this computer, or a saved
// NAS's "ssh://..." volume. Instances live for the rest of the process and NAS
// connections open on first use. Returns nullptr for an unknown NAS — callers must
// never fall back to the local disk, where the same path could mean another file.
FileSystem* filesystemFor(const std::string& volume);

inline bool isLocalVolume(const std::string& volume) { return volume.empty() || volume == "local"; }

}  // namespace storage
