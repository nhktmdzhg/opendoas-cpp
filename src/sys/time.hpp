#ifndef DOAS_SYS_TIME_HPP
#define DOAS_SYS_TIME_HPP

#include <ctime>

// Inline replacements for the timespec helpers of libopenbsd/sys-time.h, with
// identical arithmetic. Only tv_sec/tv_nsec are touched; no normalisation is
// performed on comparisons (same as the macros).
namespace sys {

constexpr bool ts_set(const struct timespec& ts) {
    return ts.tv_sec != 0 || ts.tv_nsec != 0;
}

constexpr bool ts_lt(const struct timespec& a, const struct timespec& b) {
    return a.tv_sec == b.tv_sec ? a.tv_nsec < b.tv_nsec : a.tv_sec < b.tv_sec;
}

constexpr bool ts_gt(const struct timespec& a, const struct timespec& b) {
    return a.tv_sec == b.tv_sec ? a.tv_nsec > b.tv_nsec : a.tv_sec > b.tv_sec;
}

constexpr struct timespec ts_add(const struct timespec& a, const struct timespec& b) {
    struct timespec out{};
    out.tv_sec = a.tv_sec + b.tv_sec;
    out.tv_nsec = a.tv_nsec + b.tv_nsec;
    if (out.tv_nsec >= 1000000000L) {
        out.tv_sec++;
        out.tv_nsec -= 1000000000L;
    }
    return out;
}

} // namespace sys

#endif // DOAS_SYS_TIME_HPP
