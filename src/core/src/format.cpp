#include "storage/core/format.hpp"

#include <cstdio>
#include <ctime>

namespace storage {

std::string humanSize(uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 6) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, v >= 100.0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string withThousands(uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

std::string formatTime(int64_t epochSeconds) {
    if (epochSeconds <= 0) return "-";
    const std::time_t t = static_cast<std::time_t>(epochSeconds);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

}  // namespace storage
