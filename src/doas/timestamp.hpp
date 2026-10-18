#ifndef DOAS_DOAS_TIMESTAMP_HPP
#define DOAS_DOAS_TIMESTAMP_HPP

#if defined(DOAS_PERSIST)

#include "sys/error.hpp"
#include "sys/fs.hpp"

namespace doas {

// A freshly opened timestamp file. valid is true when the timestamp inside was
// still usable, which lets the caller skip authentication entirely.
struct OpenedTimestamp {
    sys::fs::Fd fd;
    bool valid = false;
};

// Ports of timestamp.c. Failures carry the reason instead of 0/-1, so callers do
// not have to inspect errno; the module still emits the same diagnostics.
[[nodiscard]] sys::Result<OpenedTimestamp> timestamp_open(int secs);
[[nodiscard]] sys::Status timestamp_set(const sys::fs::Fd& fd, int secs);
[[nodiscard]] sys::Status timestamp_clear();

} // namespace doas

#endif // DOAS_PERSIST

#endif // DOAS_DOAS_TIMESTAMP_HPP
