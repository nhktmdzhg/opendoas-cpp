// The Error model: errno mapping, application conditions, and the byte exact
// err(3)/errc(3) diagnostics produced by die()/warn().
#include "sys/error.hpp"
#include "test_util.hpp"

#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

struct Captured {
    std::string output;
    int status = 0;
};

// Runs fn() in a child with stderr on a pipe and collects the bytes.
Captured capture_with_status(void (*fn)()) {
    int fds[2];
    if (::pipe(fds) != 0) {
        std::perror("pipe");
        std::_Exit(2);
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("fork");
        std::_Exit(2);
    }
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        fn();
        std::_Exit(99);
    }
    ::close(fds[1]);

    Captured captured;
    char buffer[512];
    for (;;) {
        const ssize_t n = ::read(fds[0], buffer, sizeof buffer);
        if (n <= 0) {
            break;
        }
        captured.output.append(buffer, static_cast<size_t>(n));
    }
    ::close(fds[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
    }
    captured.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return captured;
}

void die_errno_ctx() {
    sys::set_progname("doas-test");
    sys::die(1, sys::fail_errno(ENOENT), "/etc/doas.conf");
}

void die_errno_no_ctx() {
    sys::set_progname("doas-test");
    sys::die(1, sys::fail_errno(EACCES));
}

void die_errno_zero() {
    sys::set_progname("doas-test");
    // err(3) with errno == 0 prints "Success"; the shadow/getpwnam NULL branch
    // relied on exactly that.
    sys::die(1, sys::fail_errno(0), "getpwnam");
}

void die_message() {
    sys::set_progname("doas-test");
    sys::die(1, "Authentication failed");
}

void warn_errno_ctx() {
    sys::set_progname("doas-test");
    sys::warn(sys::fail_errno(EINVAL), "fstat(\"x\")");
    std::_Exit(0);
}

void die_domain() {
    sys::set_progname("doas-test");
    sys::die(1, sys::fail(sys::Err::kNotFound), "getpwnam");
}

} // namespace

int main() {
    // fail_errno(): generic category, errno_value() round trip.
    {
        const sys::Error e = sys::fail_errno(ENOENT);
        CHECK(e.from_errno());
        CHECK(e.code == std::errc::no_such_file_or_directory);
        CHECK_EQ(e.errno_value(), ENOENT);
        CHECK_EQ(e.code.message(), std::string("No such file or directory"));
        CHECK(e.subject.empty());
        CHECK(!e.is(sys::Err::kNotFound));
    }
    {
        const sys::Error e = sys::fail_errno(0);
        CHECK(e.from_errno());
        CHECK_EQ(e.errno_value(), 0);
        CHECK_EQ(e.code.message(), std::string("Success"));
    }

    // Application conditions live in their own category and are not errno errors.
    {
        const sys::Error e = sys::fail(sys::Err::kNotFound);
        CHECK(!e.from_errno());
        CHECK_EQ(e.errno_value(), 0);
        CHECK(e.is(sys::Err::kNotFound));
        CHECK(!e.is(sys::Err::kTooLarge));
        CHECK_EQ(sys::category().name(), std::string("doas"));
        CHECK(!e.code.message().empty());
        CHECK(e.code.message() != "Success");
    }
    {
        const sys::Error e = sys::fail(sys::Err::kNotATerminal);
        CHECK(e.is(sys::Err::kNotATerminal));
        CHECK(!e.is(sys::Err::kNotFound));
    }
    // Every application condition has a distinct message.
    {
        std::vector<std::string> messages;
        for (const sys::Err value : {sys::Err::kNotFound,
                                     sys::Err::kNotATerminal,
                                     sys::Err::kInvalidNumber,
                                     sys::Err::kTooSmall,
                                     sys::Err::kTooLarge}) {
            messages.push_back(sys::fail(value).code.message());
        }
        for (size_t i = 0; i < messages.size(); i++) {
            CHECK(!messages[i].empty());
            for (size_t j = i + 1; j < messages.size(); j++) {
                CHECK(messages[i] != messages[j]);
            }
        }
    }
    // with_subject() keeps the code and attaches the context.
    {
        const sys::Error e = sys::with_subject(sys::fail_errno(ENOENT), "/etc/doas.conf");
        CHECK_EQ(e.subject, std::string("/etc/doas.conf"));
        CHECK(e.code == std::errc::no_such_file_or_directory);
        CHECK(e.from_errno());
    }
    // Errors are copyable and independent (mutating the copy leaves the source
    // untouched), and compare through their code.
    {
        const sys::Error a = sys::fail_errno(EPERM);
        sys::Error b = a;
        b.subject = "copy";
        CHECK(a.subject.empty());
        CHECK_EQ(b.subject, std::string("copy"));
        CHECK(a.code == b.code);
        CHECK_EQ(b.errno_value(), EPERM);
    }

    // err(3)/errc(3) output parity: "<progname>: [ctx: ]<strerror>\n".
    {
        const Captured out = capture_with_status(&die_errno_ctx);
        CHECK_EQ(out.status, 1);
        CHECK_EQ(out.output, std::string("doas-test: /etc/doas.conf: No such file or directory\n"));
    }
    {
        const Captured out = capture_with_status(&die_errno_no_ctx);
        CHECK_EQ(out.status, 1);
        CHECK_EQ(out.output, std::string("doas-test: Permission denied\n"));
    }
    {
        const Captured out = capture_with_status(&die_errno_zero);
        CHECK_EQ(out.status, 1);
        CHECK_EQ(out.output, std::string("doas-test: getpwnam: Success\n"));
    }
    // errx(3) parity: no code, no word wrapping.
    {
        const Captured out = capture_with_status(&die_message);
        CHECK_EQ(out.status, 1);
        CHECK_EQ(out.output, std::string("doas-test: Authentication failed\n"));
    }
    // warn(3) does not exit.
    {
        const Captured out = capture_with_status(&warn_errno_ctx);
        CHECK_EQ(out.status, 0);
        CHECK_EQ(out.output, std::string("doas-test: fstat(\"x\"): Invalid argument\n"));
    }
    // A domain error prints its own text instead of strerror.
    {
        const Captured out = capture_with_status(&die_domain);
        CHECK_EQ(out.status, 1);
        CHECK_EQ(out.output,
                 std::string("doas-test: getpwnam: ") +
                     sys::fail(sys::Err::kNotFound).code.message() + "\n");
    }
    return test::finish("t_error");
}
