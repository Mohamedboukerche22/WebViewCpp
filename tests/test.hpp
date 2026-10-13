#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace wvctest {

struct Case {
    std::string name;
    std::function<void()> fn;
};

std::vector<Case>& registry();
inline int checks = 0;
inline int failed = 0;

struct Reg {
    Reg(std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(fn)});
    }
};

}  // namespace wvctest

#define TEST(name)                                            \
    static void name();                                       \
    static ::wvctest::Reg wvc_reg_##name(#name, name);        \
    static void name()

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++::wvctest::checks;                                                 \
        if (!(cond)) {                                                       \
            ++::wvctest::failed;                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++::wvctest::checks;                                                 \
        if (!((a) == (b))) {                                                 \
            ++::wvctest::failed;                                             \
            std::printf("  FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                    \
    } while (0)

#define CHECK_THROWS(expr)                                                   \
    do {                                                                     \
        ++::wvctest::checks;                                                 \
        bool threw = false;                                                  \
        try { (void)(expr); } catch (...) { threw = true; }                  \
        if (!threw) {                                                        \
            ++::wvctest::failed;                                             \
            std::printf("  FAIL %s:%d  expected throw: %s\n", __FILE__, __LINE__, #expr); \
        }                                                                    \
    } while (0)
