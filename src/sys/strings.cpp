#include "sys/strings.hpp"

#include <algorithm>
#include <cstring>

namespace sys {

BoundedString::BoundedString(size_t cap) : buf_(cap == 0 ? 0 : cap, '\0') {}

void BoundedString::assign(std::string_view src) {
    if (buf_.empty()) {
        len_ = 0;
        return;
    }
    const size_t copy = std::min(src.size(), buf_.size() - 1);
    std::memcpy(buf_.data(), src.data(), copy);
    len_ = copy;
    buf_[len_] = '\0';
}

bool BoundedString::append(std::string_view src) {
    const size_t total = len_ + src.size();
    if (buf_.empty()) {
        return false; // cap == 0: nothing fits and nothing is written
    }
    const size_t room = buf_.size() - len_ - 1;
    const size_t copy = std::min(src.size(), room);
    if (copy != 0) {
        std::memcpy(buf_.data() + len_, src.data(), copy);
        len_ += copy;
    }
    buf_[len_] = '\0';
    return total < buf_.size();
}

std::string_view BoundedString::view() const {
    return std::string_view(buf_.data(), len_);
}

bool utf8_valid(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto byte = static_cast<unsigned char>(s[i]);
        size_t extra = 0;
        unsigned int codepoint = 0;
        unsigned int min_codepoint = 0;

        if (byte < 0x80) {
            i++;
            continue;
        } else if (byte >= 0xc2 && byte <= 0xdf) {
            extra = 1;
            codepoint = byte & 0x1fU;
            min_codepoint = 0x80;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            extra = 2;
            codepoint = byte & 0x0fU;
            min_codepoint = 0x800;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            extra = 3;
            codepoint = byte & 0x07U;
            min_codepoint = 0x10000;
        } else {
            return false; // 0x80..0xc1 (continuation or overlong) and 0xf5..0xff
        }

        if (i + extra >= s.size()) {
            return false; // truncated sequence
        }
        for (size_t k = 1; k <= extra; k++) {
            const auto cont = static_cast<unsigned char>(s[i + k]);
            if ((cont & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6) | (cont & 0x3fU);
        }
        if (codepoint < min_codepoint || codepoint > 0x10ffffU) {
            return false;
        }
        if (codepoint >= 0xd800U && codepoint <= 0xdfffU) {
            return false; // surrogates
        }
        i += extra + 1;
    }
    return true;
}

} // namespace sys
