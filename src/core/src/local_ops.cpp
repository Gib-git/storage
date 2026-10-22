// Mutating operations for LocalFileSystem, implemented with native APIs.
#include <cerrno>
#include <cstring>
#include <memory>

#include "storage/core/file_io.hpp"
#include "storage/core/filesystem.hpp"
#include "storage/core/paths.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdio.h>
#include <sys/clonefile.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#endif
#endif

namespace storage {

namespace {
std::error_code sysErr(int e) { return std::error_code(e, std::system_category()); }
std::error_code notSupported() { return std::make_error_code(std::errc::operation_not_supported); }
std::string tempFor(const std::string& target) { return target + FileSystem::kTempSuffix; }
}  // namespace

#ifdef _WIN32

namespace {
std::wstring wpath(const std::string& utf8) {
    std::wstring p = fromUtf8(utf8).make_preferred().wstring();
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    if (p.size() >= 2 && p[1] == L':') return L"\\\\?\\" + p;
    return p;
}
std::error_code lastErr() { return sysErr(static_cast<int>(GetLastError())); }
}  // namespace

std::error_code local::renameNoReplace(const std::string& from, const std::string& to) {
    if (!MoveFileExW(wpath(from).c_str(), wpath(to).c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) return std::make_error_code(std::errc::file_exists);
        if (e == ERROR_NOT_SAME_DEVICE) return std::make_error_code(std::errc::cross_device_link);
        return sysErr(static_cast<int>(e));
    }
    return {};
}

std::error_code local::renameReplace(const std::string& from, const std::string& to) {
    if (!MoveFileExW(wpath(from).c_str(), wpath(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return lastErr();
    return {};
}

std::error_code local::copyFile(const std::string& from, const std::string& to, const std::atomic<bool>* cancel) {
    BOOL cancelFlag = FALSE;
    if (!CopyFileExW(wpath(from).c_str(), wpath(to).c_str(), nullptr, nullptr, &cancelFlag, COPY_FILE_FAIL_IF_EXISTS))
        return lastErr();
    if (cancel && cancel->load()) {
        DeleteFileW(wpath(to).c_str());
        return std::make_error_code(std::errc::operation_canceled);
    }
    HANDLE h = CreateFileW(wpath(to).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    return {};
}

void local::syncParentDir(const std::string&) {}  // MOVEFILE_WRITE_THROUGH covers this on NTFS

std::error_code LocalFileSystem::removeFile(const std::string& path) {
    if (!DeleteFileW(wpath(path).c_str())) return lastErr();
    return {};
}

std::error_code LocalFileSystem::replaceWithHardlink(const std::string& target, const std::string& source) {
    const std::string tmp = tempFor(target);
    DeleteFileW(wpath(tmp).c_str());
    if (!CreateHardLinkW(wpath(tmp).c_str(), wpath(source).c_str(), nullptr)) return lastErr();
    if (auto ec = local::renameReplace(tmp, target)) {
        DeleteFileW(wpath(tmp).c_str());
        return ec;
    }
    return {};
}

std::error_code LocalFileSystem::replaceWithClone(const std::string&, const std::string&) {
    return notSupported();  // ReFS block cloning is not wired up yet
}

#else  // POSIX

std::error_code local::renameNoReplace(const std::string& from, const std::string& to) {
#if defined(__APPLE__)
    if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) return {};
    return sysErr(errno);
#else
#if defined(SYS_renameat2)
    constexpr unsigned kNoReplace = 1;  // RENAME_NOREPLACE
    if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), kNoReplace) == 0) return {};
    if (errno != EINVAL && errno != ENOSYS) return sysErr(errno);
#endif
    // Filesystem without renameat2 support: link() refuses to overwrite, which gives
    // the same guarantee for regular files.
    struct stat st {};
    if (::lstat(from.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        if (::access(to.c_str(), F_OK) == 0) return std::make_error_code(std::errc::file_exists);
        return ::rename(from.c_str(), to.c_str()) == 0 ? std::error_code{} : sysErr(errno);
    }
    if (::link(from.c_str(), to.c_str()) != 0) return sysErr(errno);
    if (::unlink(from.c_str()) != 0) return sysErr(errno);
    return {};
#endif
}

std::error_code local::renameReplace(const std::string& from, const std::string& to) {
    if (::rename(from.c_str(), to.c_str()) != 0) return sysErr(errno);
    local::syncParentDir(to);
    return {};
}

void local::syncParentDir(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

std::error_code local::copyFile(const std::string& from, const std::string& to, const std::atomic<bool>* cancel) {
    const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return sysErr(errno);
    struct stat st {};
    if (::fstat(in, &st) != 0) {
        const int e = errno;
        ::close(in);
        return sysErr(e);
    }
    const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) {
        const int e = errno;
        ::close(in);
        return sysErr(e);
    }
    std::error_code ec;
    constexpr size_t kChunk = 1 << 20;
    auto buf = std::make_unique<char[]>(kChunk);
    for (;;) {
        if (cancel && cancel->load()) {
            ec = std::make_error_code(std::errc::operation_canceled);
            break;
        }
        const ssize_t n = ::read(in, buf.get(), kChunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            ec = sysErr(errno);
            break;
        }
        if (n == 0) break;
        for (ssize_t off = 0; off < n;) {
            const ssize_t w = ::write(out, buf.get() + off, static_cast<size_t>(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                ec = sysErr(errno);
                break;
            }
            off += w;
        }
        if (ec) break;
    }
    if (!ec) {
#if defined(__APPLE__)
        const struct timespec times[2] = {st.st_atimespec, st.st_mtimespec};
#else
        const struct timespec times[2] = {st.st_atim, st.st_mtim};
#endif
        ::futimens(out, times);
        if (::fsync(out) != 0) ec = sysErr(errno);
    }
    ::close(in);
    if (::close(out) != 0 && !ec) ec = sysErr(errno);
    if (ec) ::unlink(to.c_str());
    else syncParentDir(to);
    return ec;
}

std::error_code LocalFileSystem::removeFile(const std::string& path) {
    if (::unlink(path.c_str()) != 0) return sysErr(errno);
    local::syncParentDir(path);
    return {};
}

std::error_code LocalFileSystem::replaceWithHardlink(const std::string& target, const std::string& source) {
    const std::string tmp = tempFor(target);
    ::unlink(tmp.c_str());
    if (::link(source.c_str(), tmp.c_str()) != 0) return sysErr(errno);
    if (auto ec = local::renameReplace(tmp, target)) {
        ::unlink(tmp.c_str());
        return ec;
    }
    return {};
}

std::error_code LocalFileSystem::replaceWithClone(const std::string& target, const std::string& source) {
    const std::string tmp = tempFor(target);
    ::unlink(tmp.c_str());
#if defined(__APPLE__)
    if (::clonefile(source.c_str(), tmp.c_str(), 0) != 0) {
        const int e = errno;
        return e == ENOTSUP ? notSupported() : sysErr(e);
    }
#elif defined(__linux__) && defined(FICLONE)
    const int in = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return sysErr(errno);
    struct stat st {};
    ::fstat(in, &st);
    const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) {
        const int e = errno;
        ::close(in);
        return sysErr(e);
    }
    const int rc = ::ioctl(out, FICLONE, in);
    const int e = errno;
    if (rc == 0) {
        const struct timespec times[2] = {st.st_atim, st.st_mtim};
        ::futimens(out, times);
        ::fsync(out);
    }
    ::close(in);
    ::close(out);
    if (rc != 0) {
        ::unlink(tmp.c_str());
        return (e == EOPNOTSUPP || e == EINVAL || e == EXDEV) ? notSupported() : sysErr(e);
    }
#else
    return notSupported();
#endif
    if (auto ec = local::renameReplace(tmp, target)) {
        ::unlink(tmp.c_str());
        return ec;
    }
    return {};
}

#endif

std::error_code LocalFileSystem::replaceWithCopy(const std::string& target, const std::string& source) {
    const std::string tmp = tempFor(target);
    std::error_code ignored;
    std::filesystem::remove(fromUtf8(tmp), ignored);
    if (auto ec = local::copyFile(source, tmp)) return ec;
    if (auto ec = local::renameReplace(tmp, target)) {
        std::filesystem::remove(fromUtf8(tmp), ignored);
        return ec;
    }
    return {};
}

std::error_code LocalFileSystem::makeDirs(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(fromUtf8(dir), ec);
    return ec;
}

std::error_code LocalFileSystem::renameNoReplace(const std::string& from, const std::string& to) {
    auto ec = local::renameNoReplace(from, to);
    if (!ec) local::syncParentDir(to);
    if (ec == std::errc::cross_device_link) return std::make_error_code(std::errc::cross_device_link);
    return ec;
}

std::error_code LocalFileSystem::copyNew(const std::string& from, const std::string& to,
                                         const std::atomic<bool>* cancel) {
    return local::copyFile(from, to, cancel);
}

std::error_code LocalFileSystem::readHead(const std::string& path, size_t max, std::string& out) {
    InputFile f;
    if (auto ec = f.open(path)) return ec;
    out.resize(max);
    std::error_code ec;
    const size_t n = f.read(out.data(), max, ec);
    out.resize(n);
    return ec;
}

std::error_code LocalFileSystem::removeDir(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::remove(fromUtf8(dir), ec) && !ec) ec = std::make_error_code(std::errc::no_such_file_or_directory);
    return ec;
}

std::error_code LocalFileSystem::removeTree(const std::string& dir) {
    std::error_code ec;
    std::filesystem::remove_all(fromUtf8(dir), ec);  // removes symlinks themselves, never their targets
    return ec;
}

std::error_code LocalFileSystem::trash(const std::string& path, std::string& location) {
    return local::moveToTrash(path, location);
}

std::error_code LocalFileSystem::restore(const std::string& location, const std::string& original) {
    if (location.empty()) return notSupported();  // e.g. Windows Recycle Bin: use Explorer to restore
    std::error_code ec;
    std::filesystem::create_directories(fromUtf8(original).parent_path(), ec);
    if (auto e = local::renameNoReplace(location, original)) return e;
#if defined(__linux__)
    // Drop the freedesktop .trashinfo sidecar: <trash>/files/<name> -> <trash>/info/<name>.trashinfo
    const auto loc = fromUtf8(location);
    const auto info = loc.parent_path().parent_path() / "info" / (loc.filename().string() + ".trashinfo");
    std::filesystem::remove(info, ec);
#endif
    return {};
}

}  // namespace storage
