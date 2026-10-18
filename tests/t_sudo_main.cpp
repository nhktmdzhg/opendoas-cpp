// The sudo applet entry point: trace output and the in-process doas hand-off.
#include "sys/fs.hpp"
#include "test_util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if !defined(DOAS_SUDO_APPLET_PATH)
#error "DOAS_SUDO_APPLET_PATH must be defined by the build"
#endif

namespace {

constexpr const char* kSudoApplet = DOAS_SUDO_APPLET_PATH;

struct Outcome {
    std::string out;
    std::string err;
    int status = 0;
};

Outcome run_sudo(const std::vector<std::string>& args, std::vector<std::string> envs) {
    Outcome outcome;

    char out_path[] = "/tmp/sudo-test-out-XXXXXX";
    char err_path[] = "/tmp/sudo-test-err-XXXXXX";
    const int out_fd = mkstemp(out_path);
    const int err_fd = mkstemp(err_path);
    if (out_fd == -1 || err_fd == -1) {
        std::perror("mkstemp");
        std::exit(2);
    }

    std::fflush(nullptr);
    const pid_t child = fork();
    if (child == 0) {
        if (dup2(out_fd, STDOUT_FILENO) == -1 || dup2(err_fd, STDERR_FILENO) == -1) {
            ::_exit(126);
        }
        close(out_fd);
        close(err_fd);

        std::vector<std::string> owned(args);
        std::vector<char*> argv;
        argv.reserve(owned.size() + 1);
        for (std::string& arg : owned) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        for (std::string& env : envs) {
            putenv(env.data());
        }

        execv(kSudoApplet, argv.data());
        ::_exit(127);
    }
    if (child == -1) {
        std::perror("fork");
        std::exit(2);
    }

    int status = 0;
    while (waitpid(child, &status, 0) == -1 && errno == EINTR) {
        continue;
    }
    close(out_fd);
    close(err_fd);

    const auto out_data = sys::fs::read_file(out_path);
    const auto err_data = sys::fs::read_file(err_path);
    ::unlink(out_path);
    ::unlink(err_path);
    outcome.out = out_data.value_or(std::string{});
    outcome.err = err_data.value_or(std::string{});
    if (WIFEXITED(status)) {
        outcome.status = WEXITSTATUS(status);
    } else {
        outcome.status = 128 + WTERMSIG(status);
    }
    return outcome;
}

} // namespace

int main() {
    // Without a doas binary in PATH the applet reports it and exits with 255,
    // before printing any trace.
    {
        const Outcome outcome = run_sudo({kSudoApplet, "whoami"}, {"PATH="});
        CHECK_EQ(outcome.out, std::string(""));
        CHECK_EQ(outcome.err, std::string("doas couldn't be found on your system.\n"));
        CHECK_EQ(outcome.status, 255);
    }

    // A fake "doas" in PATH is enough for the availability check and the trace;
    // an ignored flag leaves no translated arguments, so the in-process doas
    // applet prints its usage and exits with 1.
    {
        const std::string dir = [] {
            char tmpl[] = "/tmp/sudo-path-XXXXXX";
            const char* made = mkdtemp(tmpl);
            if (made == nullptr) {
                std::perror("mkdtemp");
                std::exit(2);
            }
            return std::string(made);
        }();
        const std::string fake_doas = dir + "/doas";
        CHECK(!sys::fs::write_file(fake_doas, "#!/bin/sh\n"));
        CHECK(!sys::fs::chmod_path(fake_doas, 0755));

        const std::string path_env = "PATH=" + dir;
        const Outcome outcome = run_sudo({kSudoApplet, "--help"}, {path_env});
        CHECK_EQ(outcome.out, std::string(""));
        CHECK_EQ(outcome.err,
                 std::string("sudo <<< ") + kSudoApplet +
                     " --help\n"
                     "sudo >>> " +
                     fake_doas + "\n" +
                     "usage: doas [-Lns] [-C config] [-u user] command [args]\n");
        CHECK_EQ(outcome.status, 1);

        // With SUDO_HIDE the trace lines disappear.
        const Outcome hidden = run_sudo({kSudoApplet, "--help"}, {path_env, "SUDO_HIDE=1"});
        CHECK_EQ(hidden.out, std::string(""));
        CHECK_EQ(hidden.err,
                 std::string("usage: doas [-Lns] [-C config] [-u user] command [args]\n"));
        CHECK_EQ(hidden.status, 1);

        // -e/--edit exits successfully without running doas.
        const Outcome edited = run_sudo({kSudoApplet, "-e"}, {path_env});
        CHECK_EQ(edited.err, std::string("sudo <<< ") + kSudoApplet + " -e\n");
        CHECK_EQ(edited.status, 0);

        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }

    return test::finish("t_sudo_main");
}
