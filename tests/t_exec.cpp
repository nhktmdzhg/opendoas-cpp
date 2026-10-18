// doas::execvpe(): the PATH search, the shell fallback for ENOEXEC files and the
// diagnostics of the ported libopenbsd/execvpe.c.
#include "doas/exec.hpp"
#include "sys/fs.hpp"
#include "sys/process.hpp"
#include "test_util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

std::string make_temp_dir() {
    char tmpl[] = "/tmp/doas-exec-XXXXXX";
    const char* made = mkdtemp(tmpl);
    if (made == nullptr) {
        std::perror("mkdtemp");
        std::exit(2);
    }
    return std::string(made);
}

std::vector<char*> current_environment() {
    std::vector<char*> envp;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; entry++) {
        envp.push_back(*entry);
    }
    envp.push_back(nullptr);
    return envp;
}

// Runs execvpe() in a child; returns the child's exit status (0 when the exec
// succeeded and the executed program exited with 0).
int exec_in_child(std::string_view name, std::vector<char*> argv, std::vector<char*> envp) {
    std::fflush(nullptr);
    const pid_t child = fork();
    if (child == 0) {
        const std::error_code error = doas::execvpe(name, argv, envp);
        ::_exit(error.value() == 0 ? 99 : static_cast<int>(error.value()));
    }
    if (child == -1) {
        std::perror("fork");
        std::exit(2);
    }
    int status = 0;
    while (waitpid(child, &status, 0) == -1 && errno == EINTR) {
        continue;
    }
    if (!WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

} // namespace

int main() {
    const std::string dir = make_temp_dir();
    std::vector<char*> envp = current_environment();

    // An absolute path with a slash is executed directly.
    {
        std::vector<std::string> args = {"/bin/true"};
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (std::string& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        CHECK_EQ(exec_in_child("/bin/true", argv, envp), 0);
    }

    // PATH is taken from the environment, not from envp: "/bin" is in the process
    // environment and "true" is found there.
    {
        std::vector<std::string> args = {"true"};
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (std::string& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        CHECK_EQ(exec_in_child("true", argv, envp), 0);
    }

    // An empty name is rejected with ENOENT.
    {
        std::vector<char*> argv = {nullptr};
        const std::error_code error = doas::execvpe("", argv, envp);
        CHECK_EQ(error.value(), ENOENT);
    }

    // A missing command reports ENOENT (doas turns that into "command not found").
    {
        std::vector<char*> argv = {nullptr};
        const std::error_code error = doas::execvpe("doas-no-such-command", argv, envp);
        CHECK_EQ(error.value(), ENOENT);
    }

    // An executable file without a shebang is handed to the shell, exactly like
    // the original (argv[0] == "sh", argv[1] == the script).
    {
        const std::string script = dir + "/plain-script";
        CHECK(!sys::fs::write_file(script, "exit 42\n"));
        CHECK(!sys::fs::chmod_path(script, 0755));

        std::vector<std::string> args = {script};
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (std::string& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        CHECK_EQ(exec_in_child(script, argv, envp), 42);
    }

    // A PATH element that cannot fit into PATH_MAX is reported on stderr and the
    // search continues.
    {
        const std::string long_entry(4100, 'x');
        CHECK(sys::process::set_env("PATH", long_entry + ":/bin"));

        const std::string out_path = dir + "/exec-stderr";
        char out_template[] = "/tmp/doas-exec-stderr-XXXXXX";
        const int out_fd = mkstemp(out_template);
        CHECK(out_fd != -1);

        std::fflush(nullptr);
        const pid_t child = fork();
        if (child == 0) {
            (void)dup2(out_fd, STDERR_FILENO);
            close(out_fd);
            std::vector<std::string> args = {"/bin/true"};
            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (std::string& arg : args) {
                argv.push_back(arg.data());
            }
            argv.push_back(nullptr);
            const std::error_code error = doas::execvpe("/bin/true", argv, envp);
            ::_exit(error.value() == 0 ? 99 : static_cast<int>(error.value()));
        }
        int status = 0;
        while (waitpid(child, &status, 0) == -1 && errno == EINTR) {
            continue;
        }
        close(out_fd);
        const auto captured = sys::fs::read_file(out_template);
        ::unlink(out_template);
        CHECK_EQ(WIFEXITED(status) ? WEXITSTATUS(status) : -1, 0);
        (void)out_path;
        // The direct path has a slash, so no PATH search happened: no message.
        CHECK_EQ(captured.value_or("").size(), static_cast<size_t>(0));
    }

    // Now the same through a PATH lookup: the long element is skipped with a
    // warning and "true" is still found in /bin.
    {
        const std::string long_entry(4100, 'x');
        CHECK(sys::process::set_env("PATH", long_entry + ":/bin"));

        char out_template[] = "/tmp/doas-exec-stderr-XXXXXX";
        const int out_fd = mkstemp(out_template);
        CHECK(out_fd != -1);

        std::fflush(nullptr);
        const pid_t child = fork();
        if (child == 0) {
            (void)dup2(out_fd, STDERR_FILENO);
            close(out_fd);
            std::vector<std::string> args = {"true"};
            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (std::string& arg : args) {
                argv.push_back(arg.data());
            }
            argv.push_back(nullptr);
            const std::error_code error = doas::execvpe("true", argv, envp);
            ::_exit(error.value() == 0 ? 99 : static_cast<int>(error.value()));
        }
        int status = 0;
        while (waitpid(child, &status, 0) == -1 && errno == EINTR) {
            continue;
        }
        close(out_fd);
        const auto captured = sys::fs::read_file(out_template);
        ::unlink(out_template);
        CHECK_EQ(WIFEXITED(status) ? WEXITSTATUS(status) : -1, 0);
        const std::string expected = "execvp: " + long_entry + ": path too long\n";
        CHECK_EQ(captured.value_or(""), expected);
    }

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);

    return test::finish("t_exec");
}
