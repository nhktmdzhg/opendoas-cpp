#ifndef DOAS_SYS_PROCESS_HPP
#define DOAS_SYS_PROCESS_HPP

#include <csignal>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <utility>

namespace sys::process {

// setresuid/setresgid sentinel meaning "leave this id unchanged".
constexpr uid_t kKeepId = static_cast<uid_t>(-1);

[[nodiscard]] std::expected<pid_t, std::error_code> fork_child();

struct WaitResult {
    std::error_code error;
    int status = 0; // raw wait(2) status
};
[[nodiscard]] WaitResult wait_for(pid_t pid);

// execve(2); only returns on failure.
[[nodiscard]] std::error_code
execve_path(std::string_view path, std::span<char* const> argv, std::span<char* const> envp);

// execvp(3): PATH search using the calling process' PATH (no BSD quirks and no
// script fallback). Only returns on failure.
[[nodiscard]] std::error_code
execv_search(std::string_view name, std::span<char* const> argv, std::span<char* const> envp);

[[nodiscard]] pid_t pid();
[[nodiscard]] pid_t ppid();
[[nodiscard]] std::expected<pid_t, std::error_code> session_id();
void send_signal(pid_t target, int sig);
unsigned int sleep_seconds(unsigned int seconds);

// getenv(3)/setenv(3): the value is copied so no pointer into environ escapes.
// get_env() returns nullopt only when the variable is unset (an empty value
// yields an empty string), which is what SUDO_HIDE relies on.
[[nodiscard]] std::optional<std::string> get_env(std::string_view name);
[[nodiscard]] bool set_env(std::string_view name, std::string_view value);

// gethostname(3); returns "?" when the call fails, always NUL terminated.
[[nodiscard]] std::string hostname();

[[nodiscard]] uid_t uid();
[[nodiscard]] uid_t euid();
[[nodiscard]] gid_t gid();
[[nodiscard]] gid_t egid();
[[nodiscard]] std::error_code set_res_uid(uid_t real, uid_t effective, uid_t saved);
[[nodiscard]] std::error_code set_res_gid(gid_t real, gid_t effective, gid_t saved);
[[nodiscard]] std::error_code init_groups(std::string_view user, gid_t gid);

struct SigActionResult {
    std::error_code error;
    struct sigaction previous{};
};
[[nodiscard]] SigActionResult install_handler(int sig, void (*handler)(int));
[[nodiscard]] std::error_code restore_handler(int sig, const struct sigaction& action);

struct MaskResult {
    std::error_code error;
    sigset_t previous{};
};
[[nodiscard]] MaskResult block_all_signals();
[[nodiscard]] std::error_code unblock_signals(const sigset_t& set);

// closefrom(3) from glibc (>= 2.34).
void close_from(int lowfd);

// The sudo applet's which(): PATH lookup using access(X_OK) only (no
// regular-file check), skipping empty PATH entries like strtok(3) did.
[[nodiscard]] std::optional<std::string> which(std::string_view prog);

} // namespace sys::process

#endif // DOAS_SYS_PROCESS_HPP
