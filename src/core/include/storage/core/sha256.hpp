#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace storage {

// SHA-256 (FIPS 180-4). Chosen over faster non-cryptographic hashes because
// `sha256sum` exists on every NAS, so remote and local hashes are comparable.
class Sha256 {
public:
    using Digest = std::array<uint8_t, 32>;

    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    Digest finish();

    static std::string hex(const Digest& d);
    static std::string hexOf(const void* data, size_t len);

private:
    void block(const uint8_t* p);

    uint32_t h_[8];
    uint8_t buf_[64];
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
};

}  // namespace storage
