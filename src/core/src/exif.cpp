#include "storage/core/exif.hpp"

#include <cstdint>
#include <algorithm>
#include <cstring>

namespace storage {

namespace {

class Tiff {
public:
    Tiff(const uint8_t* p, size_t n) : p_(p), n_(n) {}

    bool init() {
        if (n_ < 8) return false;
        if (p_[0] == 'I' && p_[1] == 'I') little_ = true;
        else if (p_[0] == 'M' && p_[1] == 'M') little_ = false;
        else return false;
        return u16(2) == 42;
    }

    uint16_t u16(size_t off) const {
        if (off + 2 > n_) return 0;
        return little_ ? uint16_t(p_[off] | (p_[off + 1] << 8)) : uint16_t((p_[off] << 8) | p_[off + 1]);
    }
    uint32_t u32(size_t off) const {
        if (off + 4 > n_) return 0;
        return little_ ? uint32_t(p_[off]) | uint32_t(p_[off + 1]) << 8 | uint32_t(p_[off + 2]) << 16 | uint32_t(p_[off + 3]) << 24
                       : uint32_t(p_[off]) << 24 | uint32_t(p_[off + 1]) << 16 | uint32_t(p_[off + 2]) << 8 | uint32_t(p_[off + 3]);
    }

    // Find an ASCII tag (or the offset value of a LONG pointer tag) in the IFD at `ifd`.
    bool findAscii(size_t ifd, uint16_t tag, std::string& out) const {
        const uint16_t count = u16(ifd);
        if (ifd + 2 + size_t(count) * 12 > n_) return false;
        for (uint16_t i = 0; i < count; ++i) {
            const size_t e = ifd + 2 + size_t(i) * 12;
            if (u16(e) != tag || u16(e + 2) != 2) continue;  // type 2 = ASCII
            const uint32_t len = u32(e + 4);
            const size_t at = len <= 4 ? e + 8 : u32(e + 8);
            if (len == 0 || at + len > n_) return false;
            out.assign(reinterpret_cast<const char*>(p_ + at), len);
            while (!out.empty() && out.back() == '\0') out.pop_back();
            return true;
        }
        return false;
    }

    uint32_t findPointer(size_t ifd, uint16_t tag) const {
        const uint16_t count = u16(ifd);
        if (ifd + 2 + size_t(count) * 12 > n_) return 0;
        for (uint16_t i = 0; i < count; ++i) {
            const size_t e = ifd + 2 + size_t(i) * 12;
            if (u16(e) == tag) return u32(e + 8);
        }
        return 0;
    }

private:
    const uint8_t* p_;
    size_t n_;
    bool little_ = true;
};

int num(const std::string& s, size_t at, size_t len) {
    int v = 0;
    for (size_t i = at; i < at + len; ++i) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

// "YYYY:MM:DD HH:MM:SS"
CaptureDate parseDate(const std::string& s) {
    CaptureDate d;
    if (s.size() < 10) return d;
    d.year = num(s, 0, 4);
    d.month = num(s, 5, 2);
    d.day = num(s, 8, 2);
    if (s.size() >= 19) {
        d.hour = num(s, 11, 2);
        d.minute = num(s, 14, 2);
        d.second = num(s, 17, 2);
    }
    if (d.hour < 0 || d.minute < 0 || d.second < 0) d.hour = d.minute = d.second = 0;
    if (!d.valid()) return {};
    return d;
}

CaptureDate fromTiff(const uint8_t* p, size_t n) {
    Tiff t(p, n);
    if (!t.init()) return {};
    const uint32_t ifd0 = t.u32(4);
    if (ifd0 == 0 || ifd0 >= n) return {};
    std::string s;
    if (const uint32_t exif = t.findPointer(ifd0, 0x8769); exif && exif < n) {
        if (t.findAscii(exif, 0x9003, s)) {  // DateTimeOriginal
            if (auto d = parseDate(s); d.valid()) return d;
        }
        if (t.findAscii(exif, 0x9004, s)) {  // DateTimeDigitized
            if (auto d = parseDate(s); d.valid()) return d;
        }
    }
    if (t.findAscii(ifd0, 0x0132, s)) return parseDate(s);  // DateTime (last modified by camera/software)
    return {};
}

}  // namespace

CaptureDate readCaptureDate(const std::string& head) {
    const auto* p = reinterpret_cast<const uint8_t*>(head.data());
    const size_t n = head.size();
    if (n < 4) return {};

    // TIFF-based raw formats start with the TIFF header directly.
    if ((p[0] == 'I' && p[1] == 'I') || (p[0] == 'M' && p[1] == 'M')) return fromTiff(p, n);

    // JPEG: walk segments until APP1 "Exif\0\0".
    if (p[0] != 0xFF || p[1] != 0xD8) return {};
    size_t i = 2;
    while (i + 4 <= n) {
        if (p[i] != 0xFF) return {};
        const uint8_t marker = p[i + 1];
        if (marker == 0xD9 || marker == 0xDA) return {};  // end of image / start of scan data
        const size_t len = (size_t(p[i + 2]) << 8) | p[i + 3];
        if (len < 2) return {};
        if (marker == 0xE1 && i + 4 + 6 <= n && std::memcmp(p + i + 4, "Exif\0\0", 6) == 0) {
            const size_t start = i + 10;
            const size_t end = std::min(n, i + 2 + len);
            if (start < end) return fromTiff(p + start, end - start);
        }
        i += 2 + len;
    }
    return {};
}

}  // namespace storage
