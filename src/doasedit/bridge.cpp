#include "doasedit/bridge.hpp"

#include "doas/doas.hpp"
#include "sys/error.hpp"
#include "sys/fs.hpp"
#include "sys/io.hpp"
#include "sys/process.hpp"

#include <fcntl.h>
#include <initializer_list>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace doasedit::bridge {

namespace {

Result run(std::initializer_list<std::string_view> argv, bool capture) {
    Result result;

    auto devnull = sys::fs::open_file("/dev/null", O_RDWR);
    if (!devnull) {
        return result;
    }

    std::optional<std::pair<sys::fs::Fd, sys::fs::Fd>> pipe;
    if (capture) {
        auto made = sys::fs::make_pipe();
        if (!made) {
            return result;
        }
        pipe = std::move(*made);
    }

    // The child inherits the stdio buffers and its exit() would flush them
    // again, so everything pending is written out here.
    sys::flush_all();

    const std::expected<pid_t, std::error_code> child = sys::process::fork_child();
    if (!child) {
        return result;
    }

    if (*child == 0) {
        const int null_fd = devnull->get();
        if (sys::fs::dup2_onto(null_fd, STDIN_FILENO) ||
            sys::fs::dup2_onto(null_fd, STDERR_FILENO)) {
            ::_exit(1);
        }
        if (pipe) {
            if (sys::fs::dup2_onto(pipe->second.get(), STDOUT_FILENO)) {
                ::_exit(1);
            }
            pipe->first.reset();
            pipe->second.reset();
        } else if (sys::fs::dup2_onto(null_fd, STDOUT_FILENO)) {
            ::_exit(1);
        }
        devnull->reset();

        // Mirrors the setuid exec the original doasedit performed: the effective
        // uid goes back to 0 from the saved set user id. When the binary is not
        // installed setuid this fails and the doas applet reports "not installed
        // setuid" itself, so the failure is not fatal here.
        (void)sys::process::set_res_uid(sys::process::kKeepId, 0, sys::process::kKeepId);
        sys::set_progname("doas");

        std::vector<std::string> owned;
        owned.reserve(argv.size());
        for (const std::string_view arg : argv) {
            owned.emplace_back(arg);
        }
        std::vector<char*> cargv;
        cargv.reserve(owned.size() + 1);
        for (std::string& arg : owned) {
            cargv.push_back(arg.data());
        }
        cargv.push_back(nullptr);

        // Never returns: replace_main() either execs the target or exits through
        // an error path.
        doas::replace_main(static_cast<int>(owned.size()), cargv.data());
        ::_exit(1);
    }

    devnull->reset();
    if (pipe) {
        pipe->second.reset();
        const auto data = sys::fs::read_all(pipe->first.get());
        if (data) {
            result.out = *data;
        }
        pipe->first.reset();
    }

    const sys::process::WaitResult waited = sys::process::wait_for(*child);
    if (waited.error) {
        result.status = 1;
        return result;
    }
    if (WIFSIGNALED(waited.status)) {
        result.status = 1;
        return result;
    }
    result.status = WEXITSTATUS(waited.status);
    return result;
}

} // namespace

Result spawn_capture(std::initializer_list<std::string_view> argv) {
    return run(argv, true);
}

Result spawn_discard(std::initializer_list<std::string_view> argv) {
    return run(argv, false);
}

} // namespace doasedit::bridge
