#include <cstring>

#include "check.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed = 0, skipped = 0;
    for (const auto& c : check::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        ++run;
        const int before = check::failures();
        try {
            c.fn();
        } catch (const check::RequireFailed&) {
        } catch (const check::Skipped& s) {
            ++skipped;
            std::printf("SKIP  %s (%s)\n", c.name, s.why.c_str());
            continue;
        } catch (const std::exception& e) {
            ++check::failures();
            std::fprintf(stderr, "  unexpected exception: %s\n", e.what());
        }
        const bool ok = check::failures() == before;
        if (!ok) ++failed;
        std::printf("%s  %s\n", ok ? "ok  " : "FAIL", c.name);
    }
    std::printf("\n%d tests, %d failed, %d skipped\n", run, failed, skipped);
    return failed ? 1 : 0;
}
