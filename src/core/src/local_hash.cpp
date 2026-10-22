// Content hashing and comparison for LocalFileSystem.
#include <cstring>
#include <memory>
#include <vector>

#include "storage/core/file_io.hpp"
#include "storage/core/filesystem.hpp"
#include "storage/core/sha256.hpp"

namespace storage {

namespace {
constexpr size_t kChunk = 1 << 20;

std::error_code cancelled() { return std::make_error_code(std::errc::operation_canceled); }
}  // namespace

std::error_code LocalFileSystem::hashPartial(const std::string& path, uint64_t size, std::string& out) {
    if (size <= 2 * kPartialChunk) return hashFull(path, out, nullptr, nullptr);
    InputFile f;
    if (auto ec = f.open(path)) return ec;
    std::vector<uint8_t> buf(kPartialChunk);
    Sha256 h;
    std::error_code ec;
    for (uint64_t offset : {uint64_t{0}, size - kPartialChunk}) {
        if ((ec = f.seek(offset))) return ec;
        const size_t n = f.read(buf.data(), buf.size(), ec);
        if (ec) return ec;
        if (n != buf.size()) return std::make_error_code(std::errc::io_error);  // truncated since scan
        h.update(buf.data(), n);
    }
    out = Sha256::hex(h.finish());
    return {};
}

std::error_code LocalFileSystem::hashFull(const std::string& path, std::string& out, const std::atomic<bool>* cancel,
                                          std::atomic<uint64_t>* bytesRead) {
    InputFile f;
    if (auto ec = f.open(path)) return ec;
    auto buf = std::make_unique<uint8_t[]>(kChunk);
    Sha256 h;
    std::error_code ec;
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return cancelled();
        const size_t n = f.read(buf.get(), kChunk, ec);
        if (ec) return ec;
        if (n == 0) break;
        h.update(buf.get(), n);
        if (bytesRead) bytesRead->fetch_add(n, std::memory_order_relaxed);
    }
    out = Sha256::hex(h.finish());
    return {};
}

std::error_code LocalFileSystem::sameContent(const std::string& a, const std::string& b, bool& equal,
                                             const std::atomic<bool>* cancel) {
    InputFile fa, fb;
    if (auto ec = fa.open(a)) return ec;
    if (auto ec = fb.open(b)) return ec;
    auto ba = std::make_unique<uint8_t[]>(kChunk);
    auto bb = std::make_unique<uint8_t[]>(kChunk);
    std::error_code ec;
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return cancelled();
        const size_t na = fa.read(ba.get(), kChunk, ec);
        if (ec) return ec;
        const size_t nb = fb.read(bb.get(), kChunk, ec);
        if (ec) return ec;
        if (na != nb || std::memcmp(ba.get(), bb.get(), na) != 0) {
            equal = false;
            return {};
        }
        if (na == 0) break;
    }
    equal = true;
    return {};
}

}  // namespace storage
