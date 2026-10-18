#include "sys/fs.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {

std::error_code last_error() {
    return std::error_code(errno, std::generic_category());
}

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

int Fd::release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

void Fd::reset() {
    if (fd_ != -1) {
        (void)::close(fd_);
        fd_ = -1;
    }
}

StatResult stat_follow(std::string_view path) {
    StatResult result;
    const std::string name(path);
    if (::stat(name.c_str(), &result.st) != 0) {
        result.error = last_error();
    }
    return result;
}

StatResult fstat_fd(int fd) {
    StatResult result;
    if (::fstat(fd, &result.st) != 0) {
        result.error = last_error();
    }
    return result;
}

std::expected<Fd, std::error_code> open_file(std::string_view path, int flags, mode_t mode) {
    const std::string name(path);
    const int fd = ::open(name.c_str(), flags, mode);
    if (fd == -1) {
        return std::unexpected(last_error());
    }
    return Fd(fd);
}

std::error_code close_fd(int fd) {
    if (::close(fd) != 0) {
        return last_error();
    }
    return {};
}

std::expected<ssize_t, std::error_code> read_fd(int fd, std::span<char> buf) {
    for (;;) {
        const ssize_t got = ::read(fd, buf.data(), buf.size());
        if (got >= 0) {
            return got;
        }
        if (errno != EINTR) {
            return std::unexpected(last_error());
        }
    }
}

std::expected<std::string, std::error_code> read_all(int fd) {
    std::string out;
    std::array<char, kChunk> chunk{};
    for (;;) {
        auto got = read_fd(fd, {chunk.data(), chunk.size()});
        if (!got) {
            return std::unexpected(got.error());
        }
        if (*got == 0) {
            return out;
        }
        out.append(chunk.data(), static_cast<size_t>(*got));
    }
}

std::error_code write_all(int fd, std::string_view data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t put = ::write(fd, data.data() + written, data.size() - written);
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            return last_error();
        }
        written += static_cast<size_t>(put);
    }
    return {};
}

std::expected<std::string, std::error_code> read_file(std::string_view path) {
    auto fd = open_file(path, O_RDONLY);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    return read_all(fd->get());
}

std::error_code write_file(std::string_view path, std::string_view data) {
    auto fd = open_file(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (!fd) {
        return fd.error();
    }
    return write_all(fd->get(), data);
}

std::expected<uintmax_t, std::error_code> copy_file(const std::string& from,
                                                    const std::string& to) {
    auto src = open_file(from, O_RDONLY);
    if (!src) {
        return std::unexpected(src.error());
    }
    const StatResult src_stat = fstat_fd(src->get());
    if (src_stat.error) {
        return std::unexpected(src_stat.error);
    }

    auto dst = open_file(to, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (!dst) {
        return std::unexpected(dst.error());
    }

    uintmax_t total = 0;
    std::array<char, kChunk> chunk{};
    for (;;) {
        auto got = read_fd(src->get(), {chunk.data(), chunk.size()});
        if (!got) {
            return std::unexpected(got.error());
        }
        if (*got == 0) {
            break;
        }
        const std::error_code error =
            write_all(dst->get(), std::string_view(chunk.data(), static_cast<size_t>(*got)));
        if (error) {
            return std::unexpected(error);
        }
        total += static_cast<uintmax_t>(static_cast<unsigned long long>(*got));
    }

    // Close errors are not reported on success, matching Rust's std::fs::copy
    // (which relies on File::drop) and the C original.
    if (::fchmod(dst->get(), src_stat.st.st_mode & 07777) != 0) {
        return std::unexpected(last_error());
    }
    return total;
}

std::expected<std::pair<Fd, Fd>, std::error_code> make_pipe() {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return std::unexpected(last_error());
    }
    return std::pair<Fd, Fd>{Fd(fds[0]), Fd(fds[1])};
}

std::error_code dup2_onto(int from, int to) {
    if (::dup2(from, to) == -1) {
        return last_error();
    }
    return {};
}

std::error_code set_close_on_exec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags == -1) {
        return last_error();
    }
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
        return last_error();
    }
    return {};
}

std::error_code futimens_fd(int fd, const struct timespec* times) {
    if (::futimens(fd, times) != 0) {
        return last_error();
    }
    return {};
}

std::error_code rename_path(std::string_view from, std::string_view to) {
    const std::string src(from);
    const std::string dst(to);
    if (::rename(src.c_str(), dst.c_str()) != 0) {
        return last_error();
    }
    return {};
}

std::error_code unlink_path(std::string_view path) {
    const std::string name(path);
    if (::unlink(name.c_str()) != 0) {
        return last_error();
    }
    return {};
}

std::error_code mkdir_path(std::string_view path, mode_t mode) {
    const std::string name(path);
    if (::mkdir(name.c_str(), mode) != 0) {
        return last_error();
    }
    return {};
}

std::error_code chmod_path(std::string_view path, mode_t mode) {
    const std::string name(path);
    if (::chmod(name.c_str(), mode) != 0) {
        return last_error();
    }
    return {};
}

std::expected<std::string, std::error_code> make_temp_dir() {
    const char* tmpdir = std::getenv("TMPDIR");
    std::string tmpl = (tmpdir != nullptr && tmpdir[0] != '\0') ? tmpdir : "/tmp";
    tmpl += "/.tmpXXXXXX";

    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) == nullptr) {
        return std::unexpected(last_error());
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
