#include "sys/fs.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t kChunk = 65536;

} // namespace

namespace sys::fs {

Fd::~Fd() {
    reset();
}

Fd::Fd(Fd&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void Fd::reset() {
    if (fd_ != -1) {
        (void)::close(fd_);
        fd_ = -1;
    }
}

Result<struct stat> stat_follow(std::string_view path) {
    struct stat st{};
    const std::string name(path);
    if (::stat(name.c_str(), &st) != 0) {
        return std::unexpected(fail_errno(errno));
    }
    return st;
}

Result<struct stat> fstat_fd(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        return std::unexpected(fail_errno(errno));
    }
    return st;
}

Result<Fd> open_file(std::string_view path, int flags, mode_t mode) {
    const std::string name(path);
    const int fd = ::open(name.c_str(), flags, mode);
    if (fd == -1) {
        return std::unexpected(fail_errno(errno));
    }
    return Fd(fd);
}

Result<size_t> read_fd(int fd, std::span<char> buf) {
    for (;;) {
        const ssize_t got = ::read(fd, buf.data(), buf.size());
        if (got >= 0) {
            return static_cast<size_t>(got);
        }
        if (errno != EINTR) {
            return std::unexpected(fail_errno(errno));
        }
    }
}

Result<std::string> read_all(int fd) {
    std::string out;
    std::array<char, kChunk> chunk{};
    for (;;) {
        const auto got = read_fd(fd, {chunk.data(), chunk.size()});
        if (!got) {
            return std::unexpected(got.error());
        }
        if (*got == 0) {
            return out;
        }
        out.append(chunk.data(), *got);
    }
}

Status write_all(int fd, std::string_view data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t put = ::write(fd, data.data() + written, data.size() - written);
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(fail_errno(errno));
        }
        written += static_cast<size_t>(put);
    }
    return {};
}

Result<std::string> read_file(std::string_view path) {
    auto fd = open_file(path, O_RDONLY);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    return read_all(fd->get());
}

Status write_file(std::string_view path, std::string_view data) {
    auto fd = open_file(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    return write_all(fd->get(), data);
}

Result<uintmax_t> copy_file(const std::string& from, const std::string& to) {
    auto src = open_file(from, O_RDONLY);
    if (!src) {
        return std::unexpected(src.error());
    }
    const auto src_stat = fstat_fd(src->get());
    if (!src_stat) {
        return std::unexpected(src_stat.error());
    }

    auto dst = open_file(to, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (!dst) {
        return std::unexpected(dst.error());
    }

    uintmax_t total = 0;
    std::array<char, kChunk> chunk{};
    for (;;) {
        const auto got = read_fd(src->get(), {chunk.data(), chunk.size()});
        if (!got) {
            return std::unexpected(got.error());
        }
        if (*got == 0) {
            break;
        }
        const Status written = write_all(dst->get(), std::string_view(chunk.data(), *got));
        if (!written) {
            return std::unexpected(written.error());
        }
        total += static_cast<uintmax_t>(*got);
    }

    // Close errors are not reported on success, matching Rust's std::fs::copy
    // (which relies on File::drop) and the original C code.
    if (::fchmod(dst->get(), src_stat->st_mode & 07777) != 0) {
        return std::unexpected(fail_errno(errno));
    }
    return total;
}

Result<std::pair<Fd, Fd>> make_pipe() {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return std::unexpected(fail_errno(errno));
    }
    return std::pair<Fd, Fd>{Fd(fds[0]), Fd(fds[1])};
}

Status dup2_onto(int from, int to) {
    if (::dup2(from, to) == -1) {
        return std::unexpected(fail_errno(errno));
    }
    return {};
}

Status set_close_on_exec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags == -1) {
        return std::unexpected(fail_errno(errno));
    }
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
        return std::unexpected(fail_errno(errno));
    }
    return {};
}

Status futimens_fd(int fd, const struct timespec* times) {
    if (::futimens(fd, times) != 0) {
        return std::unexpected(fail_errno(errno));
    }
    return {};
}

Status rename_path(std::string_view from, std::string_view to) {
    const std::string src(from);
    const std::string dst(to);
    if (::rename(src.c_str(), dst.c_str()) != 0) {
        return std::unexpected(fail_errno(errno));
    }
    return {};
}

Status unlink_path(std::string_view path) {
    const std::string name(path);
    if (::unlink(name.c_str()) != 0) {
        return std::unexpected(fail_errno(errno));
    }
    return {};
}

Status mkdir_path(std::string_view path, mode_t mode) {
    const std::string name(path);
    if (::mkdir(name.c_str(), mode) != 0) {
        return std::unexpected(fail_errno(errno));
    }
    return {};
}

Status chmod_path(std::string_view path, mode_t mode) {
    const std::string name(path);
    if (::chmod(name.c_str(), mode) != 0) {
        return std::unexpected(fail_errno(errno));
    }
    return {};
}

Result<std::string> make_temp_dir() {
    const char* tmpdir = std::getenv("TMPDIR");
    std::string tmpl = (tmpdir != nullptr && tmpdir[0] != '\0') ? tmpdir : "/tmp";
    tmpl += "/.tmpXXXXXX";

    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) == nullptr) {
        return std::unexpected(fail_errno(errno));
    }
    return std::string(buf.data());
}

std::optional<std::string> current_dir() {
    std::vector<char> buf(1024);
    for (;;) {
        if (::getcwd(buf.data(), buf.size()) != nullptr) {
            return std::string(buf.data());
        }
        if (errno != ERANGE) {
            return std::nullopt;
        }
        buf.resize(buf.size() * 2);
    }
}

} // namespace sys::fs
