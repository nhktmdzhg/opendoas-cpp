#include "sys/process.hpp"

#include <cerrno>
#include <climits> // IWYU pragma: keep
#include <cstdlib>
#include <cstring>
#include <grp.h>
#include <paths.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

std::error_code last_error() {
    return std::error_code(errno, std::generic_category());
}

sys::Error fail_errno_here() {
    return sys::fail_errno(errno);
}

} // namespace

namespace sys::process {

Result<pid_t> fork_child() {
    const pid_t child = ::fork();
    if (child == -1) {
        return std::unexpected(fail_errno_here());
    }
    return child;
}

Result<WaitStatus> wait_for(pid_t child) {
    int status = 0;
    if (::waitpid(child, &status, 0) == -1) {
        return std::unexpected(fail_errno_here());
    }
    return WaitStatus(status);
}

std::error_code
execve_path(std::string_view path, std::span<char* const> argv, std::span<char* const> envp) {
    const std::string name(path);
    ::execve(name.c_str(), argv.data(), envp.data());
    return last_error();
}

std::error_code
execv_search(std::string_view name, std::span<char* const> argv, std::span<char* const> envp) {
    if (name.empty()) {
        return std::error_code(ENOENT, std::generic_category());
    }
    if (name.find('/') != std::string_view::npos) {
        (void)execve_path(name, argv, envp);
        return last_error();
    }

    bool eacces = false;
    std::error_code last;
    const std::string search = get_env("PATH").value_or(_PATH_DEFPATH);

    size_t pos = 0;
    for (;;) {
        const size_t colon = search.find(':', pos);
        const std::string_view entry = (colon == std::string::npos)
                                           ? std::string_view(search).substr(pos)
                                           : std::string_view(search).substr(pos, colon - pos);

        std::string candidate(entry);
        candidate += '/';
        candidate += name;

        ::execve(candidate.c_str(), argv.data(), envp.data());
        last = last_error();
        if (last.value() == EACCES) {
            eacces = true;
        } else if (last.value() != ENOENT && last.value() != ENOTDIR && last.value() != ESTALE) {
            return last;
        }

        if (colon == std::string::npos) {
            break;
        }
        pos = colon + 1;
    }

    if (eacces) {
        return std::error_code(EACCES, std::generic_category());
    }
    return last;
}

pid_t pid() {
    return ::getpid();
}

pid_t ppid() {
    return ::getppid();
}

Result<pid_t> session_id() {
    const pid_t sid = ::getsid(0);
    if (sid == -1) {
        return std::unexpected(fail_errno_here());
    }
    return sid;
}

void send_signal(pid_t target, int sig) {
    (void)::kill(target, sig);
}

void sleep_seconds(unsigned int seconds) {
    (void)::sleep(seconds);
}

std::optional<std::string> get_env(std::string_view name) {
    const std::string key(name);
    const char* value = ::getenv(key.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

Status set_env(std::string_view name, std::string_view value) {
    const std::string key(name);
    const std::string val(value);
    if (::setenv(key.c_str(), val.c_str(), 1) != 0) {
        return std::unexpected(fail_errno_here());
    }
    return {};
}

std::string hostname() {
    char host[HOST_NAME_MAX + 1] = {};
    if (::gethostname(host, sizeof(host)) != 0) {
        return "?";
    }
    host[sizeof(host) - 1] = '\0';
    return std::string(host);
}

uid_t uid() {
    return ::getuid();
}
uid_t euid() {
    return ::geteuid();
}
gid_t gid() {
    return ::getgid();
}
gid_t egid() {
    return ::getegid();
}

Status set_res_uid(uid_t real, uid_t effective, uid_t saved) {
    if (::setresuid(real, effective, saved) != 0) {
        return std::unexpected(fail_errno_here());
    }
    return {};
}

Status set_res_gid(gid_t real, gid_t effective, gid_t saved) {
    if (::setresgid(real, effective, saved) != 0) {
        return std::unexpected(fail_errno_here());
    }
    return {};
}

Status init_groups(std::string_view user, gid_t gid) {
    const std::string name(user);
    if (::initgroups(name.c_str(), gid) != 0) {
        return std::unexpected(fail_errno_here());
    }
    return {};
}

Result<struct sigaction> install_handler(int sig, void (*handler)(int)) {
    struct sigaction act{};
    act.sa_handler = handler;
    ::sigemptyset(&act.sa_mask);
    act.sa_flags = 0;

    struct sigaction previous{};
    if (::sigaction(sig, &act, &previous) != 0) {
        return std::unexpected(fail_errno_here());
    }
    return previous;
}

Status restore_handler(int sig, const struct sigaction& action) {
    if (::sigaction(sig, &action, nullptr) != 0) {
        return std::unexpected(fail_errno_here());
    }
    return {};
}

Result<sigset_t> block_all_signals() {
    sigset_t set;
    sigset_t previous{};
    ::sigfillset(&set);
    if (::sigprocmask(SIG_BLOCK, &set, &previous) != 0) {
        return std::unexpected(fail_errno_here());
    }
    return previous;
}

Status unblock_signals(const sigset_t& set) {
    if (::sigprocmask(SIG_UNBLOCK, &set, nullptr) != 0) {
        return std::unexpected(fail_errno_here());
    }
    return {};
}

void close_from(int lowfd) {
    ::closefrom(lowfd);
}

std::optional<std::string> which(std::string_view prog) {
    std::optional<std::string> path = get_env("PATH");
    if (!path) {
        return std::nullopt;
    }

    const std::string program(prog);
    char* save = nullptr;
    for (char* dir = ::strtok_r(path->data(), ":", &save); dir != nullptr;
         dir = ::strtok_r(nullptr, ":", &save)) {
        std::string candidate(dir);
        candidate += '/';
        candidate += program;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace sys::process
