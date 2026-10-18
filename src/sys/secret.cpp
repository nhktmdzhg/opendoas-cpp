#include "sys/secret.hpp"

#include <cstring>

namespace sys {

Secret::Secret(size_t size)
    : buf_(size == 0 ? nullptr : std::make_unique<char[]>(size)), size_(size) {
    if (size_ != 0) {
        std::memset(buf_.get(), 0, size_);
    }
}

Secret::~Secret() {
    if (size_ != 0) {
        explicit_bzero(buf_.get(), size_);
    }
}

char* Secret::data() {
    return buf_.get();
}

std::span<char> Secret::span() {
    return {buf_.get(), size_};
}

} // namespace sys
