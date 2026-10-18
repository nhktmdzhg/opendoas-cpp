#ifndef DOAS_SYS_ERROR_HPP
#define DOAS_SYS_ERROR_HPP

#include <expected>
#include <string>
#include <string_view>
#include <system_error>

namespace sys {

// Error model for the whole C++ tree.
//
//  1. A fallible operation returns Result<T> or Status. Never int 0/-1, never an
//     out-parameter, never a {error, value} struct.
//  2. Absence that is not a failure returns std::optional<T>: an unset
//     environment variable, no tty name, no PATH candidate, an unusable working
//     directory.
//  3. Plain predicates return bool: UTF-8 validation, string truncation flags.
//  4. The process boundary keeps its C shape: exit codes, argv/envp arrays, the
//     readpassphrase flag set, signal handlers and getopt.
//  5. Comparisons use Err for application conditions and std::errc for errno
//     conditions; never compare code.value() against an E* constant.
//  6. Diagnostics print code.message(), which is strerror(3)'s text for errno
//     based codes.

// Application level conditions (not errno).
enum class Err {
    kNotFound,      // no passwd/group entry, no shadow password hash
    kNotATerminal,  // readpassphrase with RPP_REQUIRE_TTY and no terminal
    kInvalidNumber, // strtonum: no digits or trailing characters
    kTooSmall,      // strtonum: below minval or negative overflow
    kTooLarge,      // strtonum: above maxval or positive overflow
};

} // namespace sys

namespace std {
template <>
struct is_error_code_enum<sys::Err> : true_type {};
} // namespace std

namespace sys {

const std::error_category& category(); // name() == "doas"
std::error_code make_error_code(Err value) noexcept;

struct Error {
    std::error_code code;
    std::string subject; // context (usually a path); empty for syscall failures

    [[nodiscard]] bool is(Err value) const {
        return code == make_error_code(value);
    }
    [[nodiscard]] bool from_errno() const {
        return code.category() == std::generic_category();
    }
    [[nodiscard]] int errno_value() const {
        return from_errno() ? code.value() : 0;
    }
};

template <class T>
using Result = std::expected<T, Error>;
using Status = std::expected<void, Error>;

[[nodiscard]] Error fail_errno(int value) noexcept;
[[nodiscard]] Error fail(Err value) noexcept;
[[nodiscard]] Error fail(std::errc value) noexcept;
[[nodiscard]] Error with_subject(Error error, std::string_view subject);

// Set the program name used as prefix by the diagnostics below. Replaces
// setprogname(3) from libopenbsd/progname.c; it also updates the glibc globals
// (program_invocation_name/program_invocation_short_name) so diagnostics emitted
// by libc itself, such as getopt(3), use the same prefix.
void set_progname(std::string_view name);

// "<progname>: " [context ": "] error.code.message() "\n", then exit(eval) for
// die(). Byte identical to err(3)/errc(3) for errno based codes.
[[noreturn]] void die(int eval, const Error& error, std::string_view context = {});
void warn(const Error& error, std::string_view context = {});

// errx(3) / warnx(3): a diagnostic without an error code.
[[noreturn]] void die(int eval, std::string_view msg);
void warn(std::string_view msg);

// syslog(3) with a pre-formatted message (priority is LOG_AUTHPRIV | LOG_*).
void log(int priority, std::string_view msg);

} // namespace sys

#endif // DOAS_SYS_ERROR_HPP
