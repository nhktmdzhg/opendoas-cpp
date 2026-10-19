// BoundedString is checked against the real strlcpy(3)/strlcat(3) provided by
// glibc, including the doas.c command-log loop.
#include "sys/strings.hpp"
#include "test_util.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr char kSentinel = '\x7f';

std::string as_string(const std::vector<char>& buf, size_t len) {
    return std::string(buf.data(), len);
}

void check_assign(size_t cap, std::string_view src) {
    // glibc strlcpy() writes at most cap - 1 bytes plus NUL, nothing beyond.
    std::vector<char> reference(cap + 8, kSentinel);
    const size_t ret = strlcpy(reference.data(), std::string(src).c_str(), cap);

    sys::BoundedString bounded(cap);
    bounded.assign(src);

    CHECK_EQ(ret, src.size());
    CHECK_EQ(std::string(bounded.view()), as_string(reference, bounded.view().size()));
    if (cap != 0) {
        CHECK(reference[bounded.view().size()] == '\0');
    }
    for (size_t i = cap == 0 ? 0 : cap; i < reference.size(); i++) {
        CHECK(reference[i] == kSentinel); // nothing written past the capacity
    }
}

void check_append(size_t cap, std::string_view initial, std::string_view src) {
    std::vector<char> reference(cap + 8, kSentinel);
    if (cap != 0) {
        std::memcpy(reference.data(), initial.data(), std::min(initial.size(), cap - 1));
        reference[std::min(initial.size(), cap - 1)] = '\0';
    }

    sys::BoundedString bounded(cap);
    bounded.assign(initial);
    const bool fits = bounded.append(src);

    const size_t ret = strlcat(reference.data(), std::string(src).c_str(), cap);
    CHECK_EQ(fits, ret < cap);
    CHECK_EQ(std::string(bounded.view()), as_string(reference, bounded.view().size()));
    if (cap != 0 && bounded.view().size() < cap) {
        CHECK(reference[bounded.view().size()] == '\0');
    }
    for (size_t i = cap == 0 ? 0 : cap; i < reference.size(); i++) {
        CHECK(reference[i] == kSentinel);
    }
}

// doas.c builds the logging line with strlcpy + a strlcat loop that stops as
// soon as one of the appends reports truncation.
std::string reference_cmdline(const std::vector<std::string>& argv, size_t cap) {
    std::vector<char> buf(cap == 0 ? 1 : cap, kSentinel);
    if (cap == 0) {
        return {};
    }
    (void)strlcpy(buf.data(), argv[0].c_str(), cap);
    for (size_t i = 1; i < argv.size(); i++) {
        if (strlcat(buf.data(), " ", cap) >= cap) {
            break;
        }
        if (strlcat(buf.data(), argv[i].c_str(), cap) >= cap) {
            break;
        }
    }
    return std::string(buf.data());
}

std::string ported_cmdline(const std::vector<std::string>& argv, size_t cap) {
    sys::BoundedString line(cap);
    if (cap == 0) {
        return {};
    }
    line.assign(argv[0]);
    for (size_t i = 1; i < argv.size(); i++) {
        if (!line.append(" ")) {
            break;
        }
        if (!line.append(argv[i])) {
            break;
        }
    }
    return std::string(line.view());
}

} // namespace

int main() {
    const std::vector<std::string_view> sources = {
        "", "a", "ab", "abc", "abcd", "abcde", "abcdefgh"};

    for (size_t cap = 0; cap <= 8; cap++) {
        for (auto src : sources) {
            check_assign(cap, src);
        }
    }

    const std::vector<std::string_view> initials = {"", "a", "ab", "abc", "abcd"};
    for (size_t cap = 0; cap <= 8; cap++) {
        for (auto initial : initials) {
            for (auto src : sources) {
                check_append(cap, initial, src);
            }
        }
    }

    // The command line built for syslog, at and around the truncation boundary.
    const std::vector<std::vector<std::string>> commands = {
        {"ls"},
        {"ls", "-l"},
        {"cat", "/etc/hosts"},
        {"echo", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
        {"a", "b", "c", "d", "e", "f"},
    };
    for (size_t cap = 0; cap <= 12; cap++) {
        for (const auto& argv : commands) {
            CHECK_EQ(reference_cmdline(argv, cap), ported_cmdline(argv, cap));
        }
    }

    // Constant time comparison of the shadow hash: equal, different in the middle,
    // different at the end, length mismatch in both directions.
    CHECK(sys::constant_time_equals("", ""));
    CHECK(sys::constant_time_equals("$6$abc$def", "$6$abc$def"));
    CHECK(!sys::constant_time_equals("$6$abc$def", "$6$abc$deg"));
    CHECK(!sys::constant_time_equals("$6$abc$def", "$6$Xbc$def"));
    CHECK(!sys::constant_time_equals("$6$abc$def", "$6$abc$de"));
    CHECK(!sys::constant_time_equals("$6$abc$de", "$6$abc$def"));
    CHECK(!sys::constant_time_equals("", "x"));
    CHECK(!sys::constant_time_equals("x", ""));

    {
        const std::vector<std::string> argv = {
            "printf", "%s", "0123456789012345678901234567890123456789"};
        CHECK_EQ(reference_cmdline(argv, 64), ported_cmdline(argv, 64));
    }

    return test::finish("t_strings");
}
