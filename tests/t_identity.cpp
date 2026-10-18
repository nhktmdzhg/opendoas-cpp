// Identity dependent matching: configurations are generated from the real uid and
// gid of the test runner, so both the permit and the deny outcomes can be checked
// without depending on the machine.
#include "sys/fs.hpp"
#include "test_util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if !defined(DOAS_APPLET_PATH)
#error "DOAS_APPLET_PATH must be defined by the build"
#endif

namespace {

constexpr const char* kApplet = DOAS_APPLET_PATH;

struct Outcome {
    std::string out;
    std::string err;
    int status = 0;
};

Outcome run_applet(const std::vector<std::string>& args) {
    Outcome outcome;

    char out_path[] = "/tmp/identity-out-XXXXXX";
    char err_path[] = "/tmp/identity-err-XXXXXX";
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

        execv(kApplet, argv.data());
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

struct MatrixCase {
    std::string config;
    std::vector<std::string> args;
    std::string expected_out;
    int expected_status = 0;
};

} // namespace

int main() {
    char tmpl[] = "/tmp/doas-identity-XXXXXX";
    const char* made = mkdtemp(tmpl);
    if (made == nullptr) {
        std::perror("mkdtemp");
        return 2;
    }
    const std::string dir(made);

    const std::string uid = std::to_string(static_cast<unsigned>(getuid()));
    const std::string gid = std::to_string(static_cast<unsigned>(getgid()));

    const std::vector<MatrixCase> matrix = {
        {"permit " + uid + "\n", {"true"}, "permit\n", 0},
        {"permit " + uid + " cmd true\n", {"true"}, "permit\n", 0},
        {"permit " + uid + " cmd true\n", {"false"}, "deny\n", 1},
        {"permit " + uid + " cmd true args a\n", {"true", "a"}, "permit\n", 0},
        {"permit " + uid + " cmd true args a\n", {"true", "b"}, "deny\n", 1},
        {"permit " + uid + " cmd true args a\n", {"true"}, "deny\n", 1},
        {"permit " + uid + " cmd true args\n", {"true"}, "permit\n", 0},
        {"permit " + uid + " cmd true args\n", {"true", "x"}, "deny\n", 1},
        {"permit :" + gid + "\n", {"true"}, "permit\n", 0},
        {"permit :4294967294\n", {"true"}, "deny\n", 1},
        {"permit " + uid + " as " + uid + "\n", {"-u", uid, "true"}, "permit\n", 0},
        {"permit " + uid + " as 0\n", {"-u", uid, "true"}, "deny\n", 1},
        {"permit nopass " + uid + "\n", {"true"}, "permit nopass\n", 0},
        {"permit nolog " + uid + "\n", {"true"}, "permit\n", 0},
        {"deny " + uid + "\npermit " + uid + "\n", {"true"}, "permit\n", 0},
        {"permit " + uid + "\ndeny " + uid + "\n", {"true"}, "deny\n", 1},
        {"permit " + uid + " cmd true args a\npermit " + uid + "\n", {"true", "b"}, "permit\n", 0},
    };

    for (size_t i = 0; i < matrix.size(); i++) {
        const std::string config = dir + "/case" + std::to_string(i) + ".conf";
        const std::error_code error = sys::fs::write_file(config, matrix[i].config);
        if (error) {
            test::report(__FILE__, __LINE__, "matrix config", "cannot write");
            continue;
        }
        std::vector<std::string> args = {kApplet, "-C", config};
        for (const std::string& extra : matrix[i].args) {
            args.push_back(extra);
        }
        const Outcome outcome = run_applet(args);
        CHECK_EQ(outcome.out, matrix[i].expected_out);
        CHECK_EQ(outcome.err, std::string(""));
        CHECK_EQ(outcome.status, matrix[i].expected_status);
        if (outcome.out != matrix[i].expected_out || outcome.status != matrix[i].expected_status) {
            std::fprintf(stderr, "  ^ identity case %zu: %s\n", i, matrix[i].config.c_str());
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);

    return test::finish("t_identity");
}
