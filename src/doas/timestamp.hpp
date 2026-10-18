#ifndef DOAS_DOAS_TIMESTAMP_HPP
#define DOAS_DOAS_TIMESTAMP_HPP

#if defined(DOAS_PERSIST)

#include "sys/fs.hpp"

#include <optional>

namespace doas {

// Ports of timestamp.c. They return -1/0 like the original functions and emit
// their diagnostics themselves.
[[nodiscard]] std::optional<sys::fs::Fd> timestamp_open(int* valid, int secs);
[[nodiscard]] int timestamp_set(int fd, int secs);
[[nodiscard]] int timestamp_clear();

} // namespace doas

#endif // DOAS_PERSIST

#endif // DOAS_DOAS_TIMESTAMP_HPP
