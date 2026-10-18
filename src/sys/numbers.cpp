#include "sys/numbers.hpp"

#include <limits>

namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

} // namespace

namespace sys {

Result<long long> parse_int(std::string_view s, long long minval, long long maxval) {
    if (minval > maxval) {
        return std::unexpected(fail(Err::kInvalidNumber));
    }

    size_t i = 0;
    while (i < s.size() && is_space(s[i])) {
        i++;
    }

    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        i++;
    }

    const size_t digits = i;
    unsigned long long magnitude = 0;
    bool overflow = false;
    const unsigned long long limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;
    while (i < s.size() && is_digit(s[i])) {
        const unsigned long long digit = static_cast<unsigned long long>(s[i] - '0');
        if (!overflow) {
            if (magnitude > (limit - digit) / 10) {
                overflow = true;
            } else {
                magnitude = (magnitude * 10) + digit;
            }
        }
        i++;
    }

    // strtonum() rejects "no digits" (numstr == ep) and trailing garbage (*ep != '\0').
    if (i == digits || i != s.size()) {
        return std::unexpected(fail(Err::kInvalidNumber));
    }

    long long value = 0;
    if (overflow) {
        value = negative ? std::numeric_limits<long long>::min()
                         : std::numeric_limits<long long>::max();
    } else if (negative) {
        value = (magnitude == 9223372036854775808ULL) ? std::numeric_limits<long long>::min()
                                                      : -static_cast<long long>(magnitude);
    } else {
        value = static_cast<long long>(magnitude);
    }

    const bool erange = overflow;
    if ((value == std::numeric_limits<long long>::min() && erange) || value < minval) {
        return std::unexpected(fail(Err::kTooSmall));
    }
    if ((value == std::numeric_limits<long long>::max() && erange) || value > maxval) {
        return std::unexpected(fail(Err::kTooLarge));
    }
    return value;
}

Result<unsigned long long> parse_u64(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && is_space(s[i])) {
        i++;
    }

    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        i++;
    }

    const size_t digits = i;
    unsigned long long value = 0;
    bool overflow = false;
    while (i < s.size() && is_digit(s[i])) {
        const unsigned long long digit = static_cast<unsigned long long>(s[i] - '0');
        if (!overflow) {
            if (value > (std::numeric_limits<unsigned long long>::max() - digit) / 10) {
                overflow = true;
            } else {
                value = (value * 10) + digit;
            }
        }
        i++;
    }

    if (i == digits) {
        return std::unexpected(fail(Err::kInvalidNumber));
    }
    if (overflow) {
        // strtoull clamps to ULLONG_MAX and sets ERANGE; timestamp.c rejects exactly that.
        return std::unexpected(fail(Err::kTooLarge));
    }

    // strtoull() negates the magnitude for a leading '-'.
    return negative ? (0ULL - value) : value;
}

} // namespace sys
