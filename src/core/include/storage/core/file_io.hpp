#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <system_error>

namespace storage {

// Minimal binary file reader with UTF-8 paths on every platform.
class InputFile {
public:
    InputFile() = default;
    ~InputFile();
    InputFile(const InputFile&) = delete;
    InputFile& operator=(const InputFile&) = delete;

    std::error_code open(const std::string& utf8Path);
    // Returns bytes read (0 at EOF); sets `ec` on error.
    size_t read(void* buf, size_t len, std::error_code& ec);
    std::error_code seek(uint64_t offset);
    void close();

private:
    std::FILE* f_ = nullptr;
};

}  // namespace storage
