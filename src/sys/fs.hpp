#ifndef DOAS_SYS_FS_HPP
#define DOAS_SYS_FS_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <utility>

namespace sys::fs {

// Sole owner of a file descriptor. Every path that opens an fd keeps it in an Fd,
// so early returns cannot leak or double close. release() hands the raw
// descriptor over to an API that takes ownership (none in this program) and get()
// gives a borrowed one.
class Fd {
  public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd();

    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    [[nodiscard]] int get() const {
        return fd_;
    }
    [[nodiscard]] bool valid() const {
        return fd_ != -1;
    }

    int release();
    void reset();

  private:
    int fd_ = -1;
};

struct StatResult {
    std::error_code error;
    struct stat st{};
};

// stat(2) / fstat(2).
[[nodiscard]] StatResult stat_follow(std::string_view path);
[[nodiscard]] StatResult fstat_fd(int fd);

// open(2). mode is only used with O_CREAT, mirroring the raw syscall.
[[nodiscard]] std::expected<Fd, std::error_code>
open_file(std::string_view path, int flags, mode_t mode = 0);

// close(2) on a borrowed descriptor (used at the timestamp API boundary).
[[nodiscard]] std::error_code close_fd(int fd);

// read(2) into a caller owned buffer; 0 means end of file.
[[nodiscard]] std::expected<ssize_t, std::error_code> read_fd(int fd, std::span<char> buf);

// Read until end of file into a string.
[[nodiscard]] std::expected<std::string, std::error_code> read_all(int fd);

// Write every byte, retrying on partial writes.
[[nodiscard]] std::error_code write_all(int fd, std::string_view data);

[[nodiscard]] std::expected<std::string, std::error_code> read_file(std::string_view path);
[[nodiscard]] std::error_code write_file(std::string_view path, std::string_view data);

// Rust's std::fs::copy equivalent: byte copy plus the permission bits of the
// source applied to the destination.
[[nodiscard]] std::expected<uintmax_t, std::error_code> copy_file(const std::string& from,
                                                                  const std::string& to);

[[nodiscard]] std::expected<std::pair<Fd, Fd>, std::error_code> make_pipe();

[[nodiscard]] std::error_code dup2_onto(int from, int to);
[[nodiscard]] std::error_code set_close_on_exec(int fd);

[[nodiscard]] std::error_code futimens_fd(int fd, const struct timespec* times);
[[nodiscard]] std::error_code rename_path(std::string_view from, std::string_view to);
[[nodiscard]] std::error_code unlink_path(std::string_view path);
[[nodiscard]] std::error_code mkdir_path(std::string_view path, mode_t mode);
[[nodiscard]] std::error_code chmod_path(std::string_view path, mode_t mode);

// mkdtemp(3) using $TMPDIR (or /tmp when unset/empty), created with mode 0700.
[[nodiscard]] std::expected<std::string, std::error_code> make_temp_dir();

[[nodiscard]] std::optional<std::string> current_dir();

} // namespace sys::fs

#endif // DOAS_SYS_FS_HPP
