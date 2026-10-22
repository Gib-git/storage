// Trash for Windows (Recycle Bin via IFileOperation) and Linux/BSD (freedesktop.org
// Trash specification). macOS lives in trash_mac.mm.
#include "storage/core/filesystem.hpp"
#include "storage/core/paths.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#elif !defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#endif

namespace storage {

#if defined(_WIN32)

std::error_code local::moveToTrash(const std::string& path, std::string& location) {
    location.clear();  // the Recycle Bin does not expose the stored name
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = SUCCEEDED(init);
    HRESULT hr = E_FAIL;
    IFileOperation* op = nullptr;
    IShellItem* item = nullptr;
    hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (SUCCEEDED(hr))
        hr = op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT |
                                   FOFX_EARLYFAILURE | FOFX_RECYCLEONDELETE);
    if (SUCCEEDED(hr)) {
        const std::wstring w = fromUtf8(path).make_preferred().wstring();
        hr = SHCreateItemFromParsingName(w.c_str(), nullptr, IID_PPV_ARGS(&item));
    }
    if (SUCCEEDED(hr)) hr = op->DeleteItem(item, nullptr);
    if (SUCCEEDED(hr)) hr = op->PerformOperations();
    BOOL aborted = FALSE;
    if (SUCCEEDED(hr) && SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && aborted) hr = E_ABORT;
    if (item) item->Release();
    if (op) op->Release();
    if (uninit) CoUninitialize();
    if (FAILED(hr)) return std::error_code(static_cast<int>(HRESULT_CODE(hr)), std::system_category());
    return {};
}

#elif !defined(__APPLE__)

namespace {

std::error_code errnoCode() { return std::error_code(errno, std::system_category()); }

std::string dirName(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? "." : s == 0 ? "/" : p.substr(0, s);
}

std::string baseName(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string percentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

bool mkdirs(const std::string& dir, mode_t mode) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ::chmod(dir.c_str(), mode);
    struct stat st {};
    return ::lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Top of the filesystem containing `path`: climb while the device stays the same.
std::string mountTop(const std::string& path, dev_t dev) {
    std::string cur = dirName(path);
    for (;;) {
        if (cur == "/") return cur;
        const std::string up = dirName(cur);
        struct stat st {};
        if (::lstat(up.c_str(), &st) != 0 || st.st_dev != dev) return cur;
        cur = up;
    }
}

}  // namespace

std::error_code local::moveToTrash(const std::string& path, std::string& location) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) return errnoCode();

    // Choose the trash directory: home trash on the same device, else $top/.Trash-$uid.
    std::string home;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) home = x;
    else if (const char* h = std::getenv("HOME"); h && *h) home = std::string(h) + "/.local/share";
    std::string trash = home.empty() ? std::string() : home + "/Trash";
    std::string infoPathPrefix;  // empty: absolute paths in .trashinfo

    struct stat hs {};
    const bool homeSameDevice = !home.empty() && ::stat(home.c_str(), &hs) == 0 && hs.st_dev == st.st_dev;
    if (!homeSameDevice) {
        const std::string top = mountTop(path, st.st_dev);
        const std::string uid = std::to_string(::getuid());
        struct stat ts {};
        const std::string shared = top + "/.Trash";
        if (::lstat(shared.c_str(), &ts) == 0 && S_ISDIR(ts.st_mode) && (ts.st_mode & S_ISVTX)) {
            trash = shared + "/" + uid;
        } else {
            trash = (top == "/" ? std::string() : top) + "/.Trash-" + uid;
        }
        infoPathPrefix = top;
    }
    if (trash.empty() || !mkdirs(trash + "/files", 0700) || !mkdirs(trash + "/info", 0700))
        return std::make_error_code(std::errc::permission_denied);

    // Reserve a unique name by creating the .trashinfo file exclusively.
    const std::string name = baseName(path);
    std::string chosen, infoFile;
    int fd = -1;
    for (int i = 1; i < 10000 && fd < 0; ++i) {
        chosen = i == 1 ? name : name + "." + std::to_string(i);
        infoFile = trash + "/info/" + chosen + ".trashinfo";
        fd = ::open(infoFile.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) return errnoCode();
    }
    if (fd < 0) return std::make_error_code(std::errc::file_exists);

    std::string original = path;
    if (!infoPathPrefix.empty() && infoPathPrefix != "/" && original.rfind(infoPathPrefix + "/", 0) == 0)
        original = original.substr(infoPathPrefix.size() + 1);
    char date[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
    const std::string info = "[Trash Info]\nPath=" + percentEncode(original) + "\nDeletionDate=" + date + "\n";
    const bool wrote = ::write(fd, info.data(), info.size()) == static_cast<ssize_t>(info.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!wrote) {
        ::unlink(infoFile.c_str());
        return std::make_error_code(std::errc::io_error);
    }

    const std::string dest = trash + "/files/" + chosen;
    if (auto ec = renameNoReplace(path, dest)) {
        ::unlink(infoFile.c_str());
        return ec;
    }
    syncParentDir(dest);
    location = dest;
    return {};
}

#endif

}  // namespace storage
