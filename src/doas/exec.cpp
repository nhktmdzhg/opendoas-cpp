// Port of libopenbsd/execvpe.c (OpenBSD execvp/execvpe). The PATH used for the
// search comes from getenv("PATH") of the calling process, not from envp.
#include "doas/exec.hpp"

#include "sys/io.hpp"
#include "sys/process.hpp"

#include <array>
#include <cerrno>
#include <climits> // IWYU pragma: keep
#include <cstring>
#include <paths.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace doas {

namespace {

constexpr size_t kPathMax = PATH_MAX;

std::error_code make_error(int value) {
    return std::error_code(value, std::generic_category());
}

// Executes path and classifies the failure exactly like the "retry:" block of
// the original: true means "try the next PATH element".
bool exec_with_retry_rules(char* path,
                           std::span<char* const> argv,
                           std::span<char* const> envp,
                           bool& eacces,
                           std::error_code& last_error) {
    (void)sys::process::execve_path(path, argv, envp);
    const int error = errno;
    last_error = make_error(error);

    switch (error) {
    case E2BIG:
        return false;
    case EISDIR:
    case ELOOP:
    case ENAMETOOLONG:
    case ENOENT:
        return true;
    case ENOEXEC: {
        // Run the file through the shell, as the kernel would not. argv is NULL
        // terminated, exactly like the original counted it.
        static char sh_name[] = "sh";
        size_t count = 0;
        while (count < argv.size() && argv[count] != nullptr) {
            count++;
        }
        std::vector<char*> shell_argv;
        shell_argv.reserve(count + 2);
        shell_argv.push_back(sh_name);
        shell_argv.push_back(path);
        for (size_t i = 1; i < count; i++) {
            shell_argv.push_back(argv[i]);
        }
        shell_argv.push_back(nullptr);
        (void)sys::process::execve_path(_PATH_BSHELL, shell_argv, envp);
        last_error = make_error(errno);
        return false;
    }
    case ENOMEM:
        return false;
    case ENOTDIR:
        return true;
    case ETXTBSY:
        // We used to retry here, but sh(1) doesn't.
        return false;
    case EACCES:
        eacces = true;
        return true;
    default:
        return false;
    }
}

} // namespace

sys::Error
execvpe(std::string_view name, std::span<char* const> argv, std::span<char* const> envp) {
    bool eacces = false;
    std::error_code last_error;

    // Do not allow null name.
    if (name.empty()) {
        return sys::fail_errno(ENOENT);
    }

    const std::string file(name);

    // If it's an absolute or relative path name, it's easy.
    if (file.find('/') != std::string::npos) {
        std::vector<char> direct(file.begin(), file.end());
        direct.push_back('\0');
        (void)exec_with_retry_rules(direct.data(), argv, envp, eacces, last_error);
        if (eacces) {
            return sys::fail_errno(EACCES);
        }
        return sys::Error{last_error, {}};
    }

    // Get the path we're searching.
    std::string path = sys::process::get_env("PATH").value_or(_PATH_DEFPATH);

    std::array<char, kPathMax> buf{};
    const size_t name_len = file.size();

    size_t pos = 0;
    for (;;) {
        std::string_view entry;
        const size_t colon = path.find(':', pos);
        if (colon == std::string::npos) {
            entry = std::string_view(path).substr(pos);
        } else {
            entry = std::string_view(path).substr(pos, colon - pos);
        }

        // It's a SHELL path: double, leading and trailing colons mean the
        // current directory.
        if (entry.empty()) {
            entry = ".";
        }
        const size_t lp = entry.size();

        // If the path is too long complain. This is a possible security issue;
        // given a way to make the path too long the user may execute the wrong
        // program.
        if (lp + name_len + 2 > buf.size()) {
            sys::err_out(std::string("execvp: ") + std::string(entry) + ": path too long\n");
            if (colon == std::string::npos) {
                break;
            }
            pos = colon + 1;
            continue;
        }

        std::memcpy(buf.data(), entry.data(), lp);
        buf[lp] = '/';
        std::memcpy(buf.data() + lp + 1, file.data(), name_len);
        buf[lp + name_len + 1] = '\0';

        if (!exec_with_retry_rules(buf.data(), argv, envp, eacces, last_error)) {
            break;
        }
        if (colon == std::string::npos) {
            break;
        }
        pos = colon + 1;
    }

    if (eacces) {
        return sys::fail_errno(EACCES);
    }
    if (!last_error) {
        return sys::fail_errno(ENOENT);
    }
    return sys::Error{last_error, {}};
}

} // namespace doas
