// Golden test: every fixture configuration is fed to the doas applet and the
// resulting stdout, stderr and exit status are compared byte for byte with the
// output of the reference C implementation (see tests/tools/regen_golden.sh).
//
//   tests/fixtures/configs/<name>.conf   configuration under test
//   tests/fixtures/cases/<name>.argv     optional command line (one argument per
//                                        line); produces the "<name>.args" case
//   tests/fixtures/golden/<case>.{out,err,code}
#include "sys/fs.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if !defined(DOAS_APPLET_PATH)
#error "DOAS_APPLET_PATH must be defined by the build"
#endif

#if !defined(DOAS_FIXTURES_DIR)
#error "DOAS_FIXTURES_DIR must be defined by the build"
#endif

namespace {

constexpr const char* kApplet = DOAS_APPLET_PATH;

struct Outcome {
    std::string out;
    std::string err;
    int status = 0;
};

// Runs the applet; the child inherits the current working directory, which is the
// fixture directory, so the relative configuration paths match the goldens.
Outcome run_applet(const std::vector<std::string>& args) {
    Outcome outcome;

    char out_path[] = "/tmp/golden-out-XXXXXX";
    char err_path[] = "/tmp/golden-err-XXXXXX";
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

bool read_golden(const std::string& path, std::string& content) {
    const auto data = sys::fs::read_file(path);
    if (!data) {
        return false;
    }
    content = *data;
    return true;
}

void check_golden(const std::string& case_name, const std::vector<std::string>& args) {
    const std::string base = "golden/" + case_name;

    std::string expected_out;
    std::string expected_err;
    std::string expected_code;
    if (!read_golden(base + ".out", expected_out) || !read_golden(base + ".err", expected_err) ||
        !read_golden(base + ".code", expected_code)) {
        test::report(
            __FILE__, __LINE__, case_name, "missing golden files: run tests/tools/regen_golden.sh");
        return;
    }

    const Outcome outcome = run_applet(args);
    CHECK_EQ(outcome.out, expected_out);
    CHECK_EQ(outcome.err, expected_err);
    CHECK_EQ(std::to_string(outcome.status) + "\n", expected_code);
    if (outcome.out != expected_out || outcome.err != expected_err ||
        std::to_string(outcome.status) + "\n" != expected_code) {
        std::fprintf(stderr, "  ^ golden case %s\n", case_name.c_str());
    }
}

std::vector<std::string> read_argv_file(const std::string& path) {
    std::vector<std::string> args;
    const auto data = sys::fs::read_file(path);
    if (!data) {
        return args;
    }
    std::string_view text(*data);
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t newline = text.find('\n', pos);
        const std::string_view line = (newline == std::string_view::npos)
                                          ? text.substr(pos)
                                          : text.substr(pos, newline - pos);
        if (!line.empty()) {
            args.emplace_back(line);
        }
        if (newline == std::string_view::npos) {
            break;
        }
        pos = newline + 1;
    }
    return args;
}

// Inputs that cannot be stored as text fixtures. They must match
// tests/tools/regen_golden.sh.
void check_synthetic_cases() {
    std::string long_line = "permit ";
    long_line.append(1030, 'a');
    long_line.push_back('\n');
    {
        const std::string path = "synthetic-longline.conf";
        (void)sys::fs::write_file(path, long_line);
        check_golden("longline", {kApplet, "-C", path});
        (void)sys::fs::unlink_path(path);
    }

    {
        const std::string path = "synthetic-nul.conf";
        const std::string content("permit nouser\0\n", 15);
        (void)sys::fs::write_file(path, content);
        check_golden("nul", {kApplet, "-C", path});
        (void)sys::fs::unlink_path(path);
    }

    check_golden("_missing", {kApplet, "-C", "configs/does-not-exist.conf"});
}

} // namespace

int main(int argc, char** argv) {
    const std::string base = argc > 1 ? argv[1] : DOAS_FIXTURES_DIR;
    if (chdir(base.c_str()) != 0) {
        std::perror("chdir");
        return 2;
    }

    std::error_code error;
    std::filesystem::directory_iterator iterator("configs", error);
    if (error) {
        std::fprintf(stderr, "cannot list fixtures in %s\n", base.c_str());
        return 2;
    }

    std::vector<std::string> names;
    for (const auto& entry : iterator) {
        if (entry.path().extension() == ".conf") {
            names.push_back(entry.path().stem().string());
        }
    }
    std::ranges::sort(names);

    for (const std::string& name : names) {
        const std::string config = "configs/" + name + ".conf";
        check_golden(name, {kApplet, "-C", config});

        const std::string argv_file = "cases/" + name + ".argv";
        if (std::filesystem::exists(argv_file, error)) {
            std::vector<std::string> args = {kApplet, "-C", config};
            for (const std::string& extra : read_argv_file(argv_file)) {
                args.push_back(extra);
            }
            check_golden(name + ".args", args);
        }
    }

    check_synthetic_cases();

    return test::finish("t_golden");
}
