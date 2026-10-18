// Differential-free expectations for sys::parse_int()/parse_u64(), derived from
// strtonum(3) (libopenbsd) and strtoull(3) semantics.
#include "sys/numbers.hpp"
#include "test_util.hpp"

#include <climits>
#include <limits>
#include <string>

using sys::IntResult;

namespace {

void expect_ok(std::string_view input, long long minval, long long maxval, long long value) {
    const IntResult result = sys::parse_int(input, minval, maxval);
    CHECK(result.error == IntResult::E::kOk);
    CHECK_EQ(result.value, value);
}

void expect_error(std::string_view input, long long minval, long long maxval, IntResult::E error) {
    const IntResult result = sys::parse_int(input, minval, maxval);
    CHECK(result.error == error);
}

} // namespace

int main() {
    // uid/gid parsing as used by doas.conf identities: parse_int(s, 0, UID_MAX - 1).
    expect_ok("0", 0, 65534, 0);
    expect_ok("1000", 0, 65534, 1000);
    expect_ok("65534", 0, 65534, 65534);
    expect_error("65535", 0, 65534, IntResult::E::kTooLarge);
    expect_error("-1", 0, 65534, IntResult::E::kTooSmall);
    expect_error("", 0, 65534, IntResult::E::kInvalid);
    expect_error("abc", 0, 65534, IntResult::E::kInvalid);
    expect_error("12abc", 0, 65534, IntResult::E::kInvalid);
    expect_error("0x10", 0, 65534, IntResult::E::kInvalid);
    expect_error("+", 0, 65534, IntResult::E::kInvalid);
    expect_error("-", 0, 65534, IntResult::E::kInvalid);
    expect_error("1 ", 0, 65534, IntResult::E::kInvalid);
    expect_error(" 1 2", 0, 65534, IntResult::E::kInvalid);

    // strtoll(3) skips leading whitespace and accepts a '+' sign: strtonum keeps
    // that behaviour, so these are accepted.
    expect_ok(" 12", 0, 65534, 12);
    expect_ok("\t12", 0, 65534, 12);
    expect_ok("\n12", 0, 65534, 12);
    expect_ok("+12", 0, 65534, 12);
    expect_ok("012", 0, 65534, 12);

    // A signed zero is zero, not an error.
    expect_ok("-0", 0, 65534, 0);

    // Limits of long long, including the clamped overflow cases.
    expect_ok("9223372036854775807", LLONG_MIN, LLONG_MAX, LLONG_MAX);
    expect_error("9223372036854775808", LLONG_MIN, LLONG_MAX, IntResult::E::kTooLarge);
    expect_ok("-9223372036854775808", LLONG_MIN, LLONG_MAX, LLONG_MIN);
    expect_error("-9223372036854775809", LLONG_MIN, LLONG_MAX, IntResult::E::kTooSmall);
    expect_error("99999999999999999999", 0, 100, IntResult::E::kTooLarge);
    expect_error("-99999999999999999999", -100, 100, IntResult::E::kTooSmall);

    // minval > maxval is invalid, as in strtonum().
    expect_error("5", 10, 4, IntResult::E::kInvalid);

    // The maxval == LLONG_MAX / minval == LLONG_MIN boundaries must not be
    // reported as errors when no overflow happened.
    expect_ok("100", 0, LLONG_MAX, 100);
    expect_ok("-100", LLONG_MIN, 0, -100);

    // parse_u64() mirrors strtoull(3) with the checks used by timestamp.c.
    {
        const auto r = sys::parse_u64("12");
        CHECK(r.ok);
        CHECK_EQ(r.value, 12ULL);
    }
    {
        const auto r = sys::parse_u64(" 12");
        CHECK(r.ok);
        CHECK_EQ(r.value, 12ULL);
    }
    {
        const auto r = sys::parse_u64("12abc");
        CHECK(r.ok); // strtoull stops at the first non digit; callers only check ep == p
        CHECK_EQ(r.value, 12ULL);
    }
    {
        const auto r = sys::parse_u64("");
        CHECK(!r.ok);
    }
    {
        const auto r = sys::parse_u64("x");
        CHECK(!r.ok);
    }
    {
        const auto r = sys::parse_u64("18446744073709551615");
        CHECK(r.ok);
        CHECK_EQ(r.value, ULLONG_MAX);
    }
    {
        const auto r = sys::parse_u64("18446744073709551616");
        CHECK(!r.ok);
    }
    {
        const auto r = sys::parse_u64("-1");
        CHECK(r.ok); // strtoull negates the magnitude
        CHECK_EQ(r.value, ULLONG_MAX);
    }

    return test::finish("t_numbers");
}
