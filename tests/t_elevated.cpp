// The real privileged operations and the in-process config validation, as far as
// they can be exercised without the setuid bit: the child raises euid and then dies
// with "not installed setuid", which is exactly the boundary these tests pin (no
// external binary is executed, no PATH lookup happens, nothing is written).
#include "doas/config.hpp"
#include "doasedit/elevated.hpp"
#include "sys/fs.hpp"
#include "sys/io.hpp"
#include "test_util.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <string>
#include <system_error>
#include <unistd.h>

namespace {

// Runs fn() with STDERR_FILENO pointing at a temporary file and returns what was
// written there (the child inherits the descriptor).
template <class Fn>
std::string capture_stderr(Fn fn) {
    char path[] = "/tmp/doas-elevated-XXXXXX";
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
    // Without the setuid bit every operation must stop inside the child with the same
    // diagnostic the applet gives, and it must never look for an external binary:
    // PATH is emptied for the whole test.
    setenv("PATH", "", 1);

    {
        const std::string err =
            capture_stderr([] { (void)doasedit::elevated::stat_path("/etc/hostname"); });
        CHECK_EQ(err, std::string("doas: not installed setuid\n"));
    }
    {
        const std::string err =
            capture_stderr([] { (void)doasedit::elevated::read_path("/etc/hostname"); });
        CHECK_EQ(err, std::string("doas: not installed setuid\n"));
    }
    // authorize_write() retries as many times as it is told to: with attempts = 3 the
    // diagnostic of all three attempts must be visible (that loop is what turns a
    // refused write into "3 incorrect password attempts").
    {
        const std::string err = capture_stderr(
            [] { (void)doasedit::elevated::authorize_write("/tmp/none", "/tmp/none", 3); });
        CHECK_EQ(err,
                 std::string("doas: not installed setuid\ndoas: not installed setuid\n"
                             "doas: not installed setuid\n"));
    }

    // A failed write must leave the target alone.
    {
        char target_path[] = "/tmp/doas-elevated-target-XXXXXX";
        const int target_fd = ::mkstemp(target_path);
        CHECK(target_fd >= 0);
        ::close(target_fd);
        CHECK(sys::fs::write_file(target_path, "keep\n").has_value());

        char source_path[] = "/tmp/doas-elevated-source-XXXXXX";
        const int source_fd = ::mkstemp(source_path);
        CHECK(source_fd >= 0);
        ::close(source_fd);
        CHECK(sys::fs::write_file(source_path, "replacement\n").has_value());

        const auto source = sys::fs::open_file(source_path, O_RDONLY);
        CHECK(source.has_value());
        if (source) {
            const std::string err = capture_stderr(
                [&] { (void)doasedit::elevated::write_path(target_path, source_path, *source); });
            CHECK_EQ(err, std::string("doas: not installed setuid\n"));
        }
        const auto content = sys::fs::read_file(target_path);
        CHECK(content.has_value());
        if (content) {
            CHECK_EQ(*content, std::string("keep\n"));
        }
        (void)::unlink(target_path);
        (void)::unlink(source_path);
    }

    // Config validation runs in this process: a valid file reports no diagnostics, a
    // broken one reports exactly the lines `doas -C <file>` prints.
    {
        char good_path[] = "/tmp/doas-elevated-good-XXXXXX";
        const int good_fd = ::mkstemp(good_path);
        CHECK(good_fd >= 0);
        ::close(good_fd);
        CHECK(sys::fs::write_file(good_path, "permit nopass root\n").has_value());
        const auto diagnostics = doas::validate_config(good_path);
        CHECK(diagnostics.has_value());
        if (diagnostics) {
            CHECK_EQ(*diagnostics, std::string());
        }
        (void)::unlink(good_path);
    }
    {
        char bad_path[] = "/tmp/doas-elevated-bad-XXXXXX";
        const int bad_fd = ::mkstemp(bad_path);
        CHECK(bad_fd >= 0);
        ::close(bad_fd);
        CHECK(sys::fs::write_file(bad_path, "permit nopass\npermit keepenv\n").has_value());
        const auto diagnostics = doas::validate_config(bad_path);
        CHECK(diagnostics.has_value());
        if (diagnostics) {
            CHECK_EQ(*diagnostics,
                     std::string("doas: syntax error at line 2\ndoas: syntax error at line 3\n"));
        }
        (void)::unlink(bad_path);
    }
    {
        const auto diagnostics = doas::validate_config("/tmp/doas-elevated-missing");
        CHECK(!diagnostics.has_value());
        if (!diagnostics) {
            CHECK(diagnostics.error().code == std::errc::no_such_file_or_directory);
        }
    }

    // memfd: the comparison copy never becomes a file, and reads back what was
    // written after rewinding it.
    {
        auto copy = sys::fs::memfd("doasedit-test");
        CHECK(copy.has_value());
        if (copy) {
            CHECK_EQ(sys::fs::write_all(copy->get(), "secret\n").has_value(), true);
            CHECK(sys::fs::rewind_fd(copy->get()).has_value());
            const auto content = sys::fs::read_all(copy->get());
            CHECK(content.has_value());
            if (content) {
                CHECK_EQ(*content, std::string("secret\n"));
            }
            // It is a real in-kernel file (a regular file with the written size),
            // just without any name in a filesystem.
            const auto st = sys::fs::fstat_fd(copy->get());
            CHECK(st.has_value());
            if (st) {
                CHECK(S_ISREG(st->st_mode) != 0);
                CHECK_EQ(st->st_size, static_cast<off_t>(7));
            }
        }
    }

    return test::finish("t_elevated");
}
