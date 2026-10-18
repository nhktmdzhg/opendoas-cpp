#include "sys/error.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>

// program_invocation_name / program_invocation_short_name are glibc extensions
// declared in <errno.h>, which <cerrno> pulls in.
#include <syslog.h>

namespace {

std::string g_progname = "doas";

// "<progname>: " [context ": "] message "\n"
void write_line(std::string_view context, const std::string& message) {
    std::string text = g_progname;
    text.append(": ");
    if (!context.empty()) {
        text.append(context);
        text.append(": ");
    }
    text.append(message);
    text.append("\n");
    std::fwrite(text.data(), 1, text.size(), stderr);
}

// Reproduces verrc() from libopenbsd/verrc.c for the legacy errno helpers.
void write_diag(std::string_view msg, bool show_errno, int code) {
    std::string text = g_progname;
    text.append(": ");
    if (!msg.empty()) {
        text.append(msg);
        if (show_errno) {
            text.append(": ");
        }
    }
    if (show_errno) {
        const char* str = std::strerror(code);
        if (str != nullptr) {
            text.append(str);
        } else {
            text.append(std::format("Unknown error {}", code));
        }
    }
    text.append("\n");
    std::fwrite(text.data(), 1, text.size(), stderr);
}

class DoasCategory : public std::error_category {
  public:
    [[nodiscard]] const char* name() const noexcept override {
        return "doas";
    }

    [[nodiscard]] std::string message(int value) const override {
        switch (static_cast<sys::Err>(value)) {
        case sys::Err::kNotFound:
            return "not found";
        case sys::Err::kNotATerminal:
            return "a tty is required";
        case sys::Err::kInvalidNumber:
            return "invalid number";
        case sys::Err::kTooSmall:
            return "number too small";
        case sys::Err::kTooLarge:
            return "number too large";
        }
        return std::format("doas error {}", value);
    }
};

} // namespace

namespace sys {

const std::error_category& category() {
    static const DoasCategory instance;
    return instance;
}

std::error_code make_error_code(Err value) noexcept {
    return std::error_code(static_cast<int>(value), category());
}

Error fail_errno(int value) noexcept {
    return Error{std::error_code(value, std::generic_category()), {}};
}

Error fail(Err value) noexcept {
    return Error{make_error_code(value), {}};
}

Error fail(std::errc value) noexcept {
    return Error{std::make_error_code(value), {}};
}

Error with_subject(Error error, std::string_view subject) {
    error.subject.assign(subject);
    return error;
}

void set_progname(std::string_view name) {
    g_progname.assign(name);
    program_invocation_name = g_progname.data();
    program_invocation_short_name = g_progname.data();
}

void die(int eval, const Error& error, std::string_view context) {
    write_line(context, error.code.message());
    std::exit(eval);
}

void warn(const Error& error, std::string_view context) {
    write_line(context, error.code.message());
}

void die(int eval, std::string_view msg) {
    write_diag(msg, false, 0);
    std::exit(eval);
}

void warn(std::string_view msg) {
    write_diag(msg, false, 0);
}

void log(int priority, std::string_view msg) {
    const std::string text(msg);
    ::syslog(priority, "%s", text.c_str());
}

} // namespace sys
