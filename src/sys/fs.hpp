#ifndef DOAS_SYS_FS_HPP
#define DOAS_SYS_FS_HPP

#include "sys/error.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>

namespace sys::fs {

// Sole owner of a file descriptor. Every path that opens an fd keeps it in an Fd,
// so early returns cannot leak or double close. get() hands out a borrowed
// descriptor; nothing in this program takes ownership of a raw fd.
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

    void reset();

  private:
    int fd_ = -1;
};

// stat(2) / fstat(2).
[[nodiscard]] Result<struct stat> stat_follow(std::string_view path);
[[nodiscard]] Result<struct stat> fstat_fd(int fd);

// open(2). mode is only used with O_CREAT, mirroring the raw syscall.
[[nodiscard]] Result<Fd> open_file(std::string_view path, int flags, mode_t mode = 0);

// read(2) into a caller owned buffer; 0 means end of file.
[[nodiscard]] Result<size_t> read_fd(int fd, std::span<char> buf);

// Read until end of file into a string.
[[nodiscard]] Result<std::string> read_all(int fd);

// Write every byte, retrying on partial writes.
[[nodiscard]] Status write_all(int fd, std::string_view data);

[[nodiscard]] Result<std::string> read_file(std::string_view path);
[[nodiscard]] Status write_file(std::string_view path, std::string_view data);

// Rust's std::fs::copy equivalent: byte copy plus the permission bits of the
// source applied to the destination.
[[nodiscard]] Result<uintmax_t> copy_file(const std::string& from, const std::string& to);

[[nodiscard]] Result<std::pair<Fd, Fd>> make_pipe();

[[nodiscard]] Status dup2_onto(int from, int to);
[[nodiscard]] Status set_close_on_exec(int fd);

[[nodiscard]] Status futimens_fd(int fd, const struct timespec* times);
[[nodiscard]] Status rename_path(std::string_view from, std::string_view to);
[[nodiscard]] Status unlink_path(std::string_view path);
[[nodiscard]] Status mkdir_path(std::string_view path, mode_t mode);
[[nodiscard]] Status chmod_path(std::string_view path, mode_t mode);

// mkdtemp(3) using $TMPDIR (or /tmp when unset/empty), created with mode 0700.
[[nodiscard]] Result<std::string> make_temp_dir();

// Deliberately degraded, like the original code which reports "(failed)":
// absence of a usable working directory is not an error.
[[nodiscard]] std::optional<std::string> current_dir();

} // namespace sys::fs

#endif // DOAS_SYS_FS_HPP
