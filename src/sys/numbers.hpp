#ifndef DOAS_SYS_NUMBERS_HPP
#define DOAS_SYS_NUMBERS_HPP

#include <string_view>

namespace sys {

// Result of parse_int(): mirrors strtonum(3) from libopenbsd/strtonum.c.
struct IntResult {
    enum class E { kOk, kInvalid, kTooSmall, kTooLarge };

    long long value = 0;
    E error = E::kOk;

    [[nodiscard]] bool has_error() const {
        return error != E::kOk;
    }
};

// Equivalent of strtonum(s, minval, maxval, &errstr):
//   * optional leading whitespace and one optional '+'/'-' sign (strtoll behaviour),
//   * at least one decimal digit, no trailing characters,
//   * a value that overflows long long is reported as too_small/too_large,
//   * minval > maxval is reported as invalid.
// errno is left untouched (no caller depends on strtonum's errno side effects).
[[nodiscard]] IntResult parse_int(std::string_view s, long long minval, long long maxval);

struct U64Result {
    unsigned long long value = 0;
    bool ok = false;
};

// Equivalent of strtoull(s, &ep, 10) with the checks used by timestamp.c:
// ok is false when nothing was converted or when the value overflowed
// (ULLONG_MAX with ERANGE). Trailing characters are accepted, like strtoull.
[[nodiscard]] U64Result parse_u64(std::string_view s);

} // namespace sys

#endif // DOAS_SYS_NUMBERS_HPP
