#pragma once

#include <string>

namespace storage {

struct CaptureDate {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    bool valid() const { return year >= 1900 && month >= 1 && month <= 12 && day >= 1 && day <= 31; }
};

// "Date taken" from the start of an image file: EXIF DateTimeOriginal in JPEG
// (APP1) or TIFF-based files (TIFF, DNG, CR2, NEF, ARW, ...). Falls back to
// DateTimeDigitized, then DateTime. `head` should hold the first ~128 KiB.
CaptureDate readCaptureDate(const std::string& head);

}  // namespace storage
