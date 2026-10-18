// Multicall dispatch: an unknown applet name reports the supported ones.
#include "sys/fs.hpp"
#include "test_util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if !defined(DOAS_COMMON_PATH)
#error "DOAS_COMMON_PATH must be defined by the build"
#endif

namespace {

constexpr const char* kBinary = DOAS_COMMON_PATH;

struct Outcome {
    std::string out;
    std::string err;
    int status = 0;
};

Outcome run(const std::vector<std::string>& args) {
    Outcome outcome;

    char out_path[] = "/tmp/dispatch-out-XXXXXX";
    char err_path[] = "/tmp/dispatch-err-XXXXXX";
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

        execv(kBinary, argv.data());
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
    // argv[0] is the bare binary name: no applet matches.
    {
        const Outcome outcome = run({kBinary});
        CHECK_EQ(outcome.out, std::string(""));
        CHECK_EQ(outcome.err,
                 std::string("doas-common: unknown applet; invoke as one of: doas, doasedit, "
                             "sudo\n"));
        CHECK_EQ(outcome.status, 1);
    }

    // An explicitly spoofed argv[0] selects an applet; a bare name that is not an
    // applet does not.
    {
        const Outcome outcome = run({"not-an-applet", "--help"});
        CHECK_EQ(outcome.err,
                 std::string("doas-common: unknown applet; invoke as one of: doas, doasedit, "
                             "sudo\n"));
        CHECK_EQ(outcome.status, 1);
    }

    return test::finish("t_dispatch");
}
