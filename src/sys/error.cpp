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

// Reproduces verrc() from libopenbsd/verrc.c:
//   "<progname>: " [msg ": "] [strerror(code)] "\n"
void write_diag(std::string_view msg, bool show_errno, int code) {
    std::string text = std::format("{}: ", g_progname);
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

} // namespace

namespace sys {

void set_progname(std::string_view name) {
    g_progname.assign(name);
    program_invocation_name = g_progname.data();
    program_invocation_short_name = g_progname.data();
}

void die(int eval, std::string_view msg) {
    write_diag(msg, false, 0);
    std::exit(eval);
}

void die_code(int eval, int code, std::string_view msg) {
    write_diag(msg, true, code);
    std::exit(eval);
}

void die_errno(int eval, std::string_view msg) {
    // Capture errno first: formatting and writing may clobber it.
    const int code = errno;
    write_diag(msg, true, code);
    std::exit(eval);
}

void warn(std::string_view msg) {
    write_diag(msg, false, 0);
}

void warn_code(int code, std::string_view msg) {
    write_diag(msg, true, code);
}

void warn_errno(std::string_view msg) {
    const int code = errno;
    write_diag(msg, true, code);
}

void log(int priority, std::string_view msg) {
    const std::string text(msg);
    ::syslog(priority, "%s", text.c_str());
}

} // namespace sys
