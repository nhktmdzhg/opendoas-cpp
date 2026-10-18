// No external test framework: both gtest and Catch2 require exceptions while this
// project is built with -fno-exceptions. Every test is a standalone executable
// using the macros below.
#ifndef DOAS_TESTS_TEST_UTIL_HPP
#define DOAS_TESTS_TEST_UTIL_HPP

#include <cstdio>
#include <string>
#include <string_view>

namespace test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void
report(std::string_view file, int line, std::string_view expr, std::string_view detail) {
    std::fprintf(stderr,
                 "FAIL %.*s:%d: %.*s\n",
                 static_cast<int>(file.size()),
                 file.data(),
                 line,
                 static_cast<int>(expr.size()),
                 expr.data());
    if (!detail.empty()) {
        std::fprintf(stderr, "     %.*s\n", static_cast<int>(detail.size()), detail.data());
    }
    ++g_failures;
}

inline int finish(std::string_view name) {
    if (g_failures == 0) {
        std::fprintf(stderr,
                     "PASS %.*s (%d checks)\n",
                     static_cast<int>(name.size()),
                     name.data(),
                     g_checks);
        return 0;
    }
    std::fprintf(stderr,
                 "FAIL %.*s (%d/%d checks failed)\n",
                 static_cast<int>(name.size()),
                 name.data(),
                 g_failures,
                 g_checks);
    return 1;
}

inline std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += '<';
            out += std::to_string(static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += '>';
        } else {
            out += c;
        }
    }
    out += "\"";
    return out;
}

// Best effort rendering of a value for failure diagnostics. Works for anything
// convertible to string_view, for booleans and for arithmetic types.
template <typename T>
std::string describe(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        return quote(std::string_view(value));
    } else if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(value);
    } else {
        return "<value>";
    }
}

} // namespace test

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++test::g_checks;                                                                          \
        if (!(cond)) {                                                                             \
            test::report(__FILE__, __LINE__, #cond, {});                                           \
        }                                                                                          \
    } while (false)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        ++test::g_checks;                                                                          \
        const auto& check_lhs = (a);                                                               \
        const auto& check_rhs = (b);                                                               \
        if (!(check_lhs == check_rhs)) {                                                           \
            test::report(__FILE__,                                                                 \
                         __LINE__,                                                                 \
                         #a " == " #b,                                                             \
                         "lhs=" + test::describe(check_lhs) +                                      \
                             " rhs=" + test::describe(check_rhs));                                 \
        }                                                                                          \
    } while (false)

#endif // DOAS_TESTS_TEST_UTIL_HPP
