#include "storage/core/paths.hpp"

#include <cstdlib>

namespace fs = std::filesystem;

namespace storage {

std::string toUtf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

fs::path fromUtf8(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

char nativeSeparator() {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

namespace {
#ifdef _WIN32
fs::path envPath(const wchar_t* name) {
    const wchar_t* v = _wgetenv(name);
    return (v && *v) ? fs::path(v) : fs::path();
}
#else
fs::path envPath(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? fs::path(v) : fs::path();
}
#endif
}  // namespace

fs::path appDataDir() {
    fs::path dir;
#if defined(_WIN32)
    dir = envPath(L"LOCALAPPDATA");
    if (dir.empty()) dir = envPath(L"APPDATA");
    dir /= "Storage";
#elif defined(__APPLE__)
    dir = envPath("HOME") / "Library" / "Application Support" / "Storage";
#else
    dir = envPath("XDG_DATA_HOME");
    if (dir.empty()) dir = envPath("HOME") / ".local" / "share";
    dir /= "storage";
#endif
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path defaultIndexPath() {
    return appDataDir() / "index.db";
}

std::string normalizeLocalPath(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::absolute(fromUtf8(path), ec);
    if (ec) p = fromUtf8(path);
    p = p.lexically_normal();
    if (!p.has_filename() && p != p.root_path()) p = p.parent_path();
    p.make_preferred();
    return toUtf8(p);
}

}  // namespace storage
