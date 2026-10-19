#ifndef DOAS_SYS_PROCESS_HPP
#define DOAS_SYS_PROCESS_HPP

#include "sys/error.hpp"

#include <csignal>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <utility>

namespace sys::process {

// setresuid/setresgid sentinel meaning "leave this id unchanged".
constexpr uid_t kKeepId = static_cast<uid_t>(-1);

[[nodiscard]] Result<pid_t> fork_child();

// Decoded wait(2) status: the raw integer stays inside this class so that no
// caller has to juggle WIFEXITED/WEXITSTATUS macros.
class WaitStatus {
  public:
    explicit WaitStatus(int raw) : raw_(raw) {}

    [[nodiscard]] bool exited() const {
        return WIFEXITED(raw_) != 0;
    }
    [[nodiscard]] bool signaled() const {
        return WIFSIGNALED(raw_) != 0;
    }
    [[nodiscard]] int exit_code() const {
        return WEXITSTATUS(raw_);
    }
    [[nodiscard]] int term_signal() const {
        return WTERMSIG(raw_);
    }
    [[nodiscard]] bool core_dumped() const {
        return WCOREDUMP(raw_) != 0;
    }

    // Shell convention: the exit code, or 128 + signal number.
    [[nodiscard]] int shell_code() const {
        return signaled() ? 128 + term_signal() : exit_code();
    }

  private:
    int raw_ = 0;
};
[[nodiscard]] Result<WaitStatus> wait_for(pid_t pid);

// execve(2) / execvp(3); they only return on failure. The error code is a plain
// std::error_code because callers compare it against std::errc directly.
[[nodiscard]] std::error_code
execve_path(std::string_view path, std::span<char* const> argv, std::span<char* const> envp);
[[nodiscard]] std::error_code
execv_search(std::string_view name, std::span<char* const> argv, std::span<char* const> envp);

[[nodiscard]] pid_t pid();
[[nodiscard]] pid_t ppid();
[[nodiscard]] Result<pid_t> session_id();
void send_signal(pid_t target, int sig);
void sleep_seconds(unsigned int seconds);

// getenv(3)/setenv(3): the value is copied so no pointer into environ escapes.
// get_env() returns nullopt only when the variable is unset (an empty value
// yields an empty string).
[[nodiscard]] std::optional<std::string> get_env(std::string_view name);
[[nodiscard]] Status set_env(std::string_view name, std::string_view value);

// gethostname(3); returns "?" when the call fails, always NUL terminated.
[[nodiscard]] std::string hostname();

[[nodiscard]] uid_t uid();
[[nodiscard]] uid_t euid();
[[nodiscard]] gid_t gid();
[[nodiscard]] gid_t egid();
[[nodiscard]] Status set_res_uid(uid_t real, uid_t effective, uid_t saved);
[[nodiscard]] Status set_res_gid(gid_t real, gid_t effective, gid_t saved);
[[nodiscard]] Status init_groups(std::string_view user, gid_t gid);

// Signal handling. install_handler() returns the previous action, which is what
// the original code restored afterwards.
[[nodiscard]] Result<struct sigaction> install_handler(int sig, void (*handler)(int));
[[nodiscard]] Status restore_handler(int sig, const struct sigaction& action);
[[nodiscard]] Result<sigset_t> block_all_signals();
[[nodiscard]] Status unblock_signals(const sigset_t& set);

// closefrom(3) from glibc (>= 2.34).
void close_from(int lowfd);

// The sudo applet's which(): PATH lookup using access(X_OK) only (no
// regular-file check), skipping empty PATH entries like strtok(3) did.
[[nodiscard]] std::optional<std::string> which(std::string_view prog);

} // namespace sys::process

#endif // DOAS_SYS_PROCESS_HPP
