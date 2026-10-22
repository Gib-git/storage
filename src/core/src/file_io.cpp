#include "storage/core/file_io.hpp"

#include <cerrno>

#include "storage/core/paths.hpp"

namespace storage {

InputFile::~InputFile() { close(); }

void InputFile::close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
}

std::error_code InputFile::open(const std::string& utf8Path) {
    close();
#ifdef _WIN32
    f_ = _wfopen(fromUtf8(utf8Path).c_str(), L"rbS");  // S: optimise for sequential access
#else
    f_ = std::fopen(utf8Path.c_str(), "rbe");          // e: O_CLOEXEC
#endif
    if (!f_) return std::error_code(errno ? errno : EIO, std::generic_category());
    std::setvbuf(f_, nullptr, _IONBF, 0);  // we read in large chunks ourselves
    return {};
}

size_t InputFile::read(void* buf, size_t len, std::error_code& ec) {
    const size_t n = std::fread(buf, 1, len, f_);
    if (n < len && std::ferror(f_)) ec = std::error_code(errno ? errno : EIO, std::generic_category());
    return n;
}

std::error_code InputFile::seek(uint64_t offset) {
#ifdef _WIN32
    const int rc = _fseeki64(f_, static_cast<__int64>(offset), SEEK_SET);
#else
    const int rc = fseeko(f_, static_cast<off_t>(offset), SEEK_SET);
#endif
    return rc == 0 ? std::error_code{} : std::error_code(errno ? errno : EIO, std::generic_category());
}

}  // namespace storage
