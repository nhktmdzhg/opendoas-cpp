// Privileged file operations for doasedit, without spawning doas or any other
// binary: the child of each operation raises its effective uid back to 0 from the
// saved set user id, runs doas's own authorization (rule matching + authentication),
// performs the syscall and reports an errno at the end of the pipe.
#include "doasedit/elevated.hpp"

#include "doas/authorize.hpp"
#include "sys/error.hpp"
#include "sys/fs.hpp"
#include "sys/io.hpp"
#include "sys/process.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace doasedit::elevated {

namespace {

constexpr size_t kChunk = 65536;

// The one authentication of a run: once a child reported a successful
// authorize_command(), the later operations skip the password prompt. Rule matching
// stays in force for every one of them.
bool g_authenticated = false;

[[nodiscard]] doas::AuthMode current_mode() {
    return g_authenticated ? doas::AuthMode::kAlreadyAuthenticated : doas::AuthMode::kAuthenticate;
}

// The command lines handed to doas's rule matching are byte for byte the ones the
// applet used to run: "cat <path>", "stat -c '%u %a' <path>" and
// "dd status=none if=<tmp> of=<target>", so existing doas.conf rules keep matching.
[[nodiscard]] std::vector<std::string> cat_args(std::string_view path) {
    return {std::string(path)};
}

[[nodiscard]] std::vector<std::string> stat_args(std::string_view path) {
    return {"-c", "%u %a", std::string(path)};
}

[[nodiscard]] std::vector<std::string> dd_args(std::string_view tmp_file, std::string_view target) {
    return {"status=none", "if=" + std::string(tmp_file), "of=" + std::string(target)};
}

// Child side: take the saved set user id back to 0 and make sure the doas
// diagnostics carry the doas name (the same message the setuid exec produced).
void enter_elevated() {
    // Same prefix as the applet: the diagnostics below belong to doas, not to
    // doasedit, and that must also hold for the "not installed setuid" message.
    sys::set_progname("doas");
    (void)sys::process::set_res_uid(sys::process::kKeepId, 0, sys::process::kKeepId);
    if (sys::process::euid() != 0) {
        sys::die(1, "not installed setuid");
    }
}

// Child side: the status word (0 or an errno) always closes the stream, so the
// parent can tell a truncated payload from a complete one.
[[nodiscard]] int child_done(int fd, int status) {
    const std::string_view word(reinterpret_cast<const char*>(&status), sizeof(status));
    (void)sys::fs::write_all(fd, word);
    return status;
}

// Parent side: `payload` is everything the child wrote before the trailing status
// word. A child that died without reporting leaves the status at 1.
int split_status(const std::string& payload, std::string* data) {
    if (payload.size() < sizeof(int)) {
        return 1;
    }
    const size_t split = payload.size() - sizeof(int);
    int status = 1;
    std::memcpy(&status, payload.data() + split, sizeof(status));
    if (data != nullptr && status == 0) {
        data->assign(payload, 0, split);
    }
    return status;
}

// Runs `body` in a child that has become root again. The body streams whatever it
// wants into `out_fd` and returns the status to report; the parent collects the
// whole stream, splits the trailing status word off and waits for the child.
template <class Body>
int run_child(Body body, std::string* payload) {
    auto pipe = sys::fs::make_pipe();
    if (!pipe) {
        return 1;
    }

    // The child inherits the stdio buffers and its exit() would flush them again,
    // so everything pending is written out here.
    sys::flush_all();

    const auto child = sys::process::fork_child();
    if (!child) {
        return 1;
    }

    if (*child == 0) {
        pipe->first.reset();
        enter_elevated();
        const int status = body(pipe->second.get());
        ::_exit(status);
    }

    pipe->second.reset();
    // Read before waiting: the child may fill the pipe while it runs.
    auto stream = sys::fs::read_all(pipe->first.get());
    pipe->first.reset();
    int status = 1;
    if (stream) {
        status = split_status(*stream, payload);
    }

    const auto waited = sys::process::wait_for(*child);
    if (!waited || waited->signaled() || waited->exit_code() != status) {
        return 1;
    }
    if (status == 0) {
        g_authenticated = true;
    }
    return status;
}

} // namespace

int authorize_write(std::string_view tmp_file, std::string_view target, int attempts) {
    const std::vector<std::string> args = dd_args(tmp_file, target);
    int status = 1;
    for (int attempt = 0; attempt < attempts; attempt++) {
        status = run_child(
            [&args](int out_fd) {
                (void)doas::authorize_command("dd", args, 0, current_mode());
                return child_done(out_fd, 0);
            },
            nullptr);
        if (status == 0) {
            return 0;
        }
    }
    return status;
}

int authorize_read(std::string_view path) {
    const std::vector<std::string> args = cat_args(path);
    return run_child(
        [&args](int out_fd) {
            (void)doas::authorize_command("cat", args, 0, current_mode());
            return child_done(out_fd, 0);
        },
        nullptr);
}

sys::Result<struct stat> stat_path(std::string_view path) {
    const std::vector<std::string> args = stat_args(path);
    std::string payload;
    const int status = run_child(
        [&args, &path](int out_fd) {
            (void)doas::authorize_command("stat", args, 0, current_mode());
            const std::string name(path);
            struct stat st{};
            if (::stat(name.c_str(), &st) != 0) {
                return child_done(out_fd, errno);
            }
            if (sys::Status written = sys::fs::write_all(
                    out_fd, std::string_view(reinterpret_cast<const char*>(&st), sizeof(st)));
                !written) {
                return EIO;
            }
            return child_done(out_fd, 0);
        },
        &payload);

    if (status != 0) {
        return std::unexpected(sys::fail_errno(status));
    }
    if (payload.size() != sizeof(struct stat)) {
        return std::unexpected(sys::fail_errno(EIO));
    }
    struct stat st{};
    std::memcpy(&st, payload.data(), sizeof(st));
    return st;
}

Text read_path(std::string_view path) {
    const std::vector<std::string> args = cat_args(path);
    std::string payload;
    const int status = run_child(
        [&args, &path](int out_fd) {
            (void)doas::authorize_command("cat", args, 0, current_mode());
            const std::string name(path);
            const auto opened = sys::fs::open_file(name, O_RDONLY);
            if (!opened) {
                return child_done(out_fd, opened.error().errno_value());
            }

            std::array<char, kChunk> buffer{};
            for (;;) {
                const auto got = sys::fs::read_fd(opened->get(), {buffer.data(), buffer.size()});
                if (!got) {
                    return child_done(out_fd, got.error().errno_value());
                }
                if (*got == 0) {
                    break;
                }
                if (sys::Status written =
                        sys::fs::write_all(out_fd, std::string_view(buffer.data(), *got));
                    !written) {
                    return child_done(out_fd, EIO);
                }
            }
            return child_done(out_fd, 0);
        },
        &payload);

    Text result;
    result.status = status;
    if (status == 0) {
        result.out = std::move(payload);
    }
    return result;
}

int write_path(std::string_view path, std::string_view tmp_file, const sys::fs::Fd& source) {
    const std::vector<std::string> args = dd_args(tmp_file, path);
    const int source_fd = source.get();
    return run_child(
        [&args, &path, source_fd](int out_fd) {
            (void)doas::authorize_command("dd", args, 0, current_mode());
            const std::string name(path);
            const auto opened = sys::fs::open_file(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (!opened) {
                return child_done(out_fd, opened.error().errno_value());
            }

            std::array<char, kChunk> buffer{};
            for (;;) {
                const auto got = sys::fs::read_fd(source_fd, {buffer.data(), buffer.size()});
                if (!got) {
                    return child_done(out_fd, got.error().errno_value());
                }
                if (*got == 0) {
                    break;
                }
                if (sys::Status written =
                        sys::fs::write_all(opened->get(), std::string_view(buffer.data(), *got));
                    !written) {
                    return child_done(out_fd, written.error().errno_value());
                }
            }
            return child_done(out_fd, 0);
        },
        nullptr);
}

} // namespace doasedit::elevated
