#pragma once

#include <cstdint>
#include <string>

namespace storage {

// 1536 -> "1.5 KB" (binary multiples, labelled the way Finder/Explorer users expect).
std::string humanSize(uint64_t bytes);

// 1234567 -> "1,234,567"
std::string withThousands(uint64_t n);

// Local time as "2026-10-21 23:00", or "-" for 0.
std::string formatTime(int64_t epochSeconds);

}  // namespace storage
