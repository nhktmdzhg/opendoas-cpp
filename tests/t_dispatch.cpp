// Multicall dispatch: an unknown applet name reports the supported ones.
#include "sys/fs.hpp"
#include "test_util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <tuple>
#include <unistd.h>
#include <utility>
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
    const std::string common_help =
        "opendoas-cpp dev\n"
        "\n"
        "Usage: doas-common [-h|--help] [-v|--version] [-l|--list]\n"
        "\n"
        "This binary is a multicall binary; invoke it through one of its applet symlinks:\n"
        "  doas      execute a command as another user\n"
        "  doasedit  edit files as root using an unprivileged editor\n"
        "  sudo      translate a sudo command line and run it through doas\n";
    const std::string applet_list = "doas\texecute a command as another user\n"
                                    "doasedit\tedit files as root using an unprivileged editor\n"
                                    "sudo\ttranslate a sudo command line and run it through doas\n";
    const std::string doas_help =
        "doas (opendoas-cpp) - execute commands as another user\n"
        "\n"
        "Usage: doas [-Lhnsv] [-C config] [-u user] command [args]\n"
        "\n"
        "Arguments:\n"
        "  command [args]  Command to run as another user\n"
        "\n"
        "Options:\n"
        "  -C config       Read rules from config instead of /etc/doas.conf\n"
        "  -L              Clear the persist timestamp\n"
        "  -n              Non-interactive: fail instead of asking for a password\n"
        "  -s              Run the shell from $SHELL or passwd(5)\n"
        "  -u user         Run the command as user\n"
        "  -h, --help      Print help\n"
        "  -v, --version   Print version\n";
    const std::string doasedit_help =
        "doasedit (opendoas-cpp) - edit files as root with an unprivileged editor\n"
        "\n"
        "Usage: doasedit [-h|--help] [-v|--version] <files>...\n"
        "\n"
        "Arguments:\n"
        "  <files>...  Files to edit\n"
        "\n"
        "Options:\n"
        "  -h, --help     Print help\n"
        "  -v, --version  Print version\n";
    const std::string sudo_help =
        "sudo (opendoas-cpp) - translate a sudo command line into doas arguments\n"
        "\n"
        "Usage: sudo [-h|--help] [-v|--version] [sudo options] [command [args]...]\n"
        "\n"
        "Options:\n"
        "  -h, --help     Print help\n"
        "  -v, --version  Print version\n"
        "\n"
        "Every other sudo option is translated to its doas equivalent or ignored; what may\n"
        "run is decided by doas.conf(5).\n";

    // doas-common invoked as itself: the three informational flags only.
    {
        const std::vector<std::pair<std::string, std::string>> cases = {
            {"-h", common_help},
            {"--help", common_help},
            {"-v", "opendoas-cpp dev\n"},
            {"--version", "opendoas-cpp dev\n"},
            {"-l", applet_list},
            {"--list", applet_list},
        };
        for (const auto& [flag, expected] : cases) {
            const Outcome outcome = run({"doas-common", flag});
            CHECK_EQ(outcome.out, expected);
            CHECK_EQ(outcome.err, std::string(""));
            CHECK_EQ(outcome.status, 0);
        }
    }

    // doas-common with no arguments or an unknown one: the usage line.
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{kBinary}, std::vector<std::string>{"doas-common", "--bogus"}}) {
        const Outcome outcome = run(args);
        CHECK_EQ(outcome.out, std::string(""));
        CHECK_EQ(outcome.err,
                 std::string("usage: doas-common [-h|--help] [-v|--version] [-l|--list]\n"));
        CHECK_EQ(outcome.status, 1);
    }

    // Other names are not applets at all.
    {
        const Outcome outcome = run({"not-an-applet", "--help"});
        CHECK_EQ(outcome.err,
                 std::string("doas-common: unknown applet; invoke as one of: doas, doasedit, "
                             "sudo\n"));
        CHECK_EQ(outcome.status, 1);
    }

    // Every applet answers help and version itself.
    {
        const std::vector<std::tuple<std::string, std::string, std::string>> cases = {
            {"doas", "-h", doas_help},
            {"doas", "--help", doas_help},
            {"doasedit", "-h", doasedit_help},
            {"doasedit", "--help", doasedit_help},
            {"sudo", "-h", sudo_help},
            {"sudo", "--help", sudo_help},
        };
        for (const auto& [applet, flag, expected] : cases) {
            const Outcome outcome = run({applet, flag});
            CHECK_EQ(outcome.out, expected);
            CHECK_EQ(outcome.err, std::string(""));
            CHECK_EQ(outcome.status, 0);
        }
    }
    {
        const std::vector<std::pair<std::string, std::string>> cases = {
            {"doas", "doas (opendoas-cpp) dev\n"},
            {"doasedit", "doasedit (opendoas-cpp) dev\n"},
            {"sudo", "sudo (opendoas-cpp) dev\n"},
        };
        for (const auto& [applet, expected] : cases) {
            for (const char* flag : {"-v", "--version"}) {
                const Outcome outcome = run({applet, flag});
                CHECK_EQ(outcome.out, expected);
                CHECK_EQ(outcome.err, std::string(""));
                CHECK_EQ(outcome.status, 0);
            }
        }
    }

    // The usage path of the doas applet is unchanged: an unknown option still fails.
    {
        const Outcome outcome = run({"doas", "-Z"});
        CHECK_EQ(outcome.out, std::string(""));
        CHECK(outcome.err.find("invalid option") != std::string::npos);
        CHECK(outcome.err.find("usage: doas [-Lhnsv]") != std::string::npos);
        CHECK_EQ(outcome.status, 1);
    }

    return test::finish("t_dispatch");
}
