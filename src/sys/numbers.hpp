#ifndef DOAS_SYS_NUMBERS_HPP
#define DOAS_SYS_NUMBERS_HPP

#include "sys/error.hpp"

#include <string_view>

namespace sys {

// Equivalent of strtonum(s, minval, maxval, &errstr):
//   * optional leading whitespace and one optional '+'/'-' sign (strtoll behaviour),
//   * at least one decimal digit, no trailing characters,
//   * a value that overflows long long is reported as kTooSmall/kTooLarge,
//   * minval > maxval is reported as kInvalidNumber.
// errno is left untouched (no caller depends on strtonum's errno side effects).
[[nodiscard]] Result<long long> parse_int(std::string_view s, long long minval, long long maxval);

// Equivalent of strtoull(s, &ep, 10) with the checks used by timestamp.c: a
// failed conversion and an overflowing value are both errors. Trailing
// characters are accepted, like strtoull.
[[nodiscard]] Result<unsigned long long> parse_u64(std::string_view s);

} // namespace sys

#endif // DOAS_SYS_NUMBERS_HPP
