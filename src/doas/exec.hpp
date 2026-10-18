#ifndef DOAS_DOAS_EXEC_HPP
#define DOAS_DOAS_EXEC_HPP

#include "sys/error.hpp"

#include <span>
#include <string_view>
#include <system_error>

namespace doas {

// Port of libopenbsd/execvpe.c. The PATH used for the search comes from
// getenv("PATH") of the calling process, not from envp (glibc's execvpe agrees
// on that point but differs on several edge cases doas relies on).
// Only returns on failure; the error carries the last errno.
[[nodiscard]] sys::Error
execvpe(std::string_view name, std::span<char* const> argv, std::span<char* const> envp);

} // namespace doas

#endif // DOAS_DOAS_EXEC_HPP
