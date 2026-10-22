#include "storage/core/filesystem.hpp"
#include "storage/core/paths.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace storage {

#ifdef _WIN32

namespace {

// Extended-length path so deep trees beyond MAX_PATH still work.
std::wstring longPath(const std::string& utf8) {
    std::wstring p = fromUtf8(utf8).make_preferred().wstring();
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    if (p.size() >= 2 && p[1] == L':') return L"\\\\?\\" + p;
    return p;
}

int64_t toUnix(const FILETIME& ft) {
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    if (t < 116444736000000000ULL) return 0;
    return static_cast<int64_t>((t - 116444736000000000ULL) / 10000000ULL);
}

NodeKind kindFromAttributes(DWORD attrs) {
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return NodeKind::Symlink;  // symlinks + junctions: never followed
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return NodeKind::Directory;
    return NodeKind::File;
}

std::error_code lastError() {
    return std::error_code(static_cast<int>(GetLastError()), std::system_category());
}

}  // namespace

std::error_code LocalFileSystem::stat(const std::string& path, DirEntry& out) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(longPath(path).c_str(), GetFileExInfoStandard, &data)) return lastError();
    out.name = toUtf8(fromUtf8(path).filename());
    out.kind = kindFromAttributes(data.dwFileAttributes);
    out.size = out.kind == NodeKind::File
                   ? ((static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow)
                   : 0;
    out.mtime = toUnix(data.ftLastWriteTime);
    out.dev = 0;
    out.ino = 0;
    out.nlink = 1;
    return {};
}

std::error_code LocalFileSystem::list(const std::string& dir, std::vector<DirEntry>& out) {
    out.clear();
    std::wstring pattern = longPath(dir);
    if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
    pattern.push_back(L'*');

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return {};  // empty directory (e.g. drive root)
        return std::error_code(static_cast<int>(err), std::system_category());
    }
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        DirEntry e;
        e.name = toUtf8(std::filesystem::path(n));
        e.kind = kindFromAttributes(fd.dwFileAttributes);
        e.size = e.kind == NodeKind::File ? ((static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow) : 0;
        e.mtime = toUnix(fd.ftLastWriteTime);
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    const DWORD err = GetLastError();
    FindClose(h);
    if (err != ERROR_NO_MORE_FILES) return std::error_code(static_cast<int>(err), std::system_category());
    return {};
}

char LocalFileSystem::separator() const { return '\\'; }

#else  // POSIX

namespace {

void fill(DirEntry& e, const struct stat& st) {
    if (S_ISDIR(st.st_mode)) e.kind = NodeKind::Directory;
    else if (S_ISREG(st.st_mode)) e.kind = NodeKind::File;
    else if (S_ISLNK(st.st_mode)) e.kind = NodeKind::Symlink;
    else e.kind = NodeKind::Other;
    e.size = (e.kind == NodeKind::File) ? static_cast<uint64_t>(st.st_size) : 0;
    e.mtime = static_cast<int64_t>(st.st_mtime);
    e.dev = static_cast<uint64_t>(st.st_dev);
    e.ino = static_cast<uint64_t>(st.st_ino);
    e.nlink = static_cast<uint32_t>(st.st_nlink);
}

std::error_code errnoCode() { return std::error_code(errno, std::system_category()); }

}  // namespace

std::error_code LocalFileSystem::stat(const std::string& path, DirEntry& out) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) return errnoCode();
    const size_t slash = path.find_last_of('/');
    out.name = slash == std::string::npos ? path : path.substr(slash + 1);
    fill(out, st);
    return {};
}

std::error_code LocalFileSystem::list(const std::string& dir, std::vector<DirEntry>& out) {
    out.clear();
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return errnoCode();
    DIR* dp = ::fdopendir(fd);
    if (!dp) {
        const auto ec = errnoCode();
        ::close(fd);
        return ec;
    }
    while (const dirent* de = ::readdir(dp)) {
        const char* n = de->d_name;
        if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) continue;
        struct stat st {};
        if (::fstatat(fd, n, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;  // vanished mid-scan
        DirEntry e;
        e.name = n;
        fill(e, st);
        out.push_back(std::move(e));
    }
    ::closedir(dp);  // also closes fd
    return {};
}

char LocalFileSystem::separator() const { return '/'; }

#endif

std::string FileSystem::parentOf(const std::string& path) const {
    size_t end = path.size();
    while (end > 1 && (path[end - 1] == '/' || path[end - 1] == '\\')) --end;
    const size_t s = path.find_last_of("/\\", end - 1);
    if (s == std::string::npos) return {};
    if (s == 0) return path.substr(0, 1);
    if (s == 2 && path.size() > 1 && path[1] == ':') return path.substr(0, 3);  // keep the drive root ("C:\\")
    return path.substr(0, s);
}

std::string LocalFileSystem::join(const std::string& dir, const std::string& name) const {
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
    return dir + separator() + name;
}

}  // namespace storage
