#pragma once

// Minimal self-registering test harness (TEST_CASE / CHECK / REQUIRE / SKIP).
// Run the binary with a substring argument to filter test names.

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct RequireFailed {};
struct Skipped {
    std::string why;
};

inline int& failures() {
    static int f = 0;
    return f;
}

inline void report(const char* file, int line, const char* expr) {
    ++failures();
    std::fprintf(stderr, "  %s:%d: CHECK failed: %s\n", file, line, expr);
}

}  // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)

#define TEST_CASE(name)                                                                       \
    static void CHECK_CAT(check_test_, __LINE__)();                                           \
    static const ::check::Registrar CHECK_CAT(check_reg_, __LINE__)(name, &CHECK_CAT(check_test_, __LINE__)); \
    static void CHECK_CAT(check_test_, __LINE__)()

#define CHECK(...)                                                    \
    do {                                                              \
        if (!(__VA_ARGS__)) ::check::report(__FILE__, __LINE__, #__VA_ARGS__); \
    } while (0)

#define REQUIRE(...)                                                  \
    do {                                                              \
        if (!(__VA_ARGS__)) {                                         \
            ::check::report(__FILE__, __LINE__, #__VA_ARGS__);        \
            throw ::check::RequireFailed{};                           \
        }                                                             \
    } while (0)

#define SKIP(msg) throw ::check::Skipped{msg}
