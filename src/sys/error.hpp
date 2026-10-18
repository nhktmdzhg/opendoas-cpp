#ifndef DOAS_SYS_ERROR_HPP
#define DOAS_SYS_ERROR_HPP

#include <string_view>

namespace sys {

// Set the program name used as prefix by the diagnostics below. Replaces
// setprogname(3) from libopenbsd/progname.c; it also updates the glibc globals
// (program_invocation_name/program_invocation_short_name) so diagnostics
// emitted by libc itself, such as getopt(3), use the same prefix.
void set_progname(std::string_view name);

// errx(3) / err(3) / errc(3) equivalents, always exiting with the given status.
[[noreturn]] void die(int eval, std::string_view msg);
[[noreturn]] void die_code(int eval, int code, std::string_view msg);
[[noreturn]] void die_errno(int eval, std::string_view msg);

// warnx(3) / warn(3) / errc(3)-without-exit equivalents.
void warn(std::string_view msg);
void warn_code(int code, std::string_view msg);
void warn_errno(std::string_view msg);

// syslog(3) with a pre-formatted message (priority is LOG_AUTHPRIV | LOG_*).
void log(int priority, std::string_view msg);

} // namespace sys

#endif // DOAS_SYS_ERROR_HPP
