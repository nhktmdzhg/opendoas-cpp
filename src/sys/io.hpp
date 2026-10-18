#ifndef DOAS_SYS_IO_HPP
#define DOAS_SYS_IO_HPP

#include <cstdio>
#include <optional>
#include <string_view>

namespace sys {

// Write raw bytes to stdout / stderr (stdio, so ordering and buffering match the
// original fprintf/printf based code).
void out(std::string_view s);
void err_out(std::string_view s);

// Like err_out(), but reports whether the write succeeded; pam.c checks the
// fprintf(3) return value and aborts the conversation when it fails.
[[nodiscard]] bool err_out_checked(std::string_view s);

// Flush every stdio stream. Mandatory in the parent right before fork(): a child
// inherits the stdio buffers and would otherwise duplicate pending output.
void flush_all();

// stdio helpers used by modules that still need a FILE*.
std::FILE* stdout_file();
std::FILE* stderr_file();

// RAII wrapper around stdio streams (the config parser reads with getc/ungetc).
class FileHandle {
  public:
    FileHandle() = default;
    ~FileHandle();
    FileHandle(FileHandle&& other) noexcept;
    FileHandle& operator=(FileHandle&& other) noexcept;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] static std::optional<FileHandle> open_read(std::string_view path);

    [[nodiscard]] std::FILE* raw() const {
        return fp_;
    }
    [[nodiscard]] int fd() const;

  private:
    explicit FileHandle(std::FILE* fp) : fp_(fp) {}

    std::FILE* fp_ = nullptr;
};

} // namespace sys

#endif // DOAS_SYS_IO_HPP
