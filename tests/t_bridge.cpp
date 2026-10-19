// The real doasedit bridge: the child must hand the doas applet an argv whose
// first slot is the program name, otherwise getopt(3) parses the command's own
// arguments ("test -e f", "dd status=none ...") as doas options.
#include "doasedit/bridge.hpp"
#include "sys/io.hpp"
#include "test_util.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

// Runs fn() with STDERR_FILENO pointing at a temporary file and returns what the
// bridge child wrote there.
template <class Fn>
std::string capture_child_stderr(Fn fn) {
    char path[] = "/tmp/doas-bridge-XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0) {
        std::perror("mkstemp");
        std::exit(2);
    }
    ::unlink(path);

    sys::flush_all();
    const int saved = ::dup(STDERR_FILENO);
    if (saved < 0 || ::dup2(fd, STDERR_FILENO) < 0) {
        std::perror("dup2");
        std::exit(2);
    }

    fn();

    sys::flush_all();
    if (::dup2(saved, STDERR_FILENO) < 0) {
        std::perror("dup2");
        std::exit(2);
    }
    ::close(saved);

    std::string out;
    char buf[512];
    for (;;) {
        const ssize_t got = ::pread(fd, buf, sizeof(buf), static_cast<off_t>(out.size()));
        if (got <= 0) {
            break;
        }
        out.append(buf, static_cast<size_t>(got));
    }
    ::close(fd);
    return out;
}

} // namespace

int main() {
    // "test -e <path>" is the command shape that used to be mangled the worst: its
    // -e/-f/-d argument was taken as a doas option, so doas printed an invalid
    // option diagnostic plus the usage line and never ran the command.
    const std::string discard_stderr =
        capture_child_stderr([] { (void)doasedit::bridge::spawn_discard({"test", "-e", "/"}); });
    CHECK(discard_stderr.find("invalid option") == std::string::npos);
    CHECK(discard_stderr.find("usage:") == std::string::npos);
    // A silently empty stderr is the other failure mode this test guards: the child
    // used to exit before doas ever ran (an inverted dup2 status check), which left
    // "doas dd …" failing with nothing to explain why.
    CHECK(!discard_stderr.empty());

    const std::string capture_stderr =
        capture_child_stderr([] { (void)doasedit::bridge::spawn_capture({"test", "-f", "/"}); });
    CHECK(capture_stderr.find("invalid option") == std::string::npos);
    CHECK(capture_stderr.find("usage:") == std::string::npos);
    CHECK(!capture_stderr.empty());

    // "dd status=none if=… of=…" used to run the command "status=none"; the child
    // must now fail (or succeed) on the real command instead.
    const std::string dd_stderr = capture_child_stderr(
        [] { (void)doasedit::bridge::spawn_discard({"dd", "status=none", "if=/dev/null"}); });
    CHECK(dd_stderr.find("status=none:") == std::string::npos);

    return test::finish("t_bridge");
}
