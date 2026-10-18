#include "sys/io.hpp"

#include <cerrno>
#include <cstdio>
#include <string>

namespace sys {

void out(std::string_view s) {
    if (!s.empty()) {
        (void)std::fwrite(s.data(), 1, s.size(), stdout);
    }
}

void err_out(std::string_view s) {
    if (!s.empty()) {
        (void)std::fwrite(s.data(), 1, s.size(), stderr);
    }
}

Status err_out_checked(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    if (std::fwrite(s.data(), 1, s.size(), stderr) != s.size()) {
        return std::unexpected(fail_errno(errno));
    }
    return {};
}

void flush_all() {
    std::fflush(nullptr);
}

std::FILE* stdout_file() {
    return stdout;
}

std::FILE* stderr_file() {
    return stderr;
}

FileHandle::~FileHandle() {
    if (fp_ != nullptr) {
        std::fclose(fp_);
    }
}

FileHandle::FileHandle(FileHandle&& other) noexcept : fp_(other.fp_) {
    other.fp_ = nullptr;
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
    if (this != &other) {
        if (fp_ != nullptr) {
            std::fclose(fp_);
        }
        fp_ = other.fp_;
        other.fp_ = nullptr;
    }
    return *this;
}

std::optional<FileHandle> FileHandle::open_read(std::string_view path) {
    const std::string name(path);
    std::FILE* fp = std::fopen(name.c_str(), "r");
    if (fp == nullptr) {
        return std::nullopt;
    }
    return FileHandle(fp);
}

int FileHandle::fd() const {
    return fp_ == nullptr ? -1 : fileno(fp_);
}

} // namespace sys
