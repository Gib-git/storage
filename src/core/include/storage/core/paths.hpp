#pragma once

#include <filesystem>
#include <string>

namespace storage {

// UTF-8 <-> filesystem path conversion that is correct on Windows too.
std::string toUtf8(const std::filesystem::path& p);
std::filesystem::path fromUtf8(const std::string& s);

// Preferred separator for local paths on this platform.
char nativeSeparator();

// Per-user directory for the index database, journals and settings.
// macOS: ~/Library/Application Support/Storage
// Linux: $XDG_DATA_HOME/storage (default ~/.local/share/storage)
// Windows: %LOCALAPPDATA%\Storage
std::filesystem::path appDataDir();

// Default index database location inside appDataDir().
std::filesystem::path defaultIndexPath();

// Normalise a user-supplied local path: absolute, lexically normal, no trailing separator.
std::string normalizeLocalPath(const std::string& path);

}  // namespace storage
