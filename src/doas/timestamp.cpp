/*
 * Copyright (c) 2020 Duncan Overbruck <mail@duncano.de>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * See the original timestamp.c for the security requirements of this file:
 * timestamps must not be reachable through symlinks, must use more than one
 * clock, and must not trust the user controllable stdin/stdout/stderr to
 * determine the controlling terminal.
 */
#include "doas/timestamp.hpp"

#if defined(DOAS_PERSIST)

#include "sys/error.hpp"
#include "sys/fs.hpp"
#include "sys/numbers.hpp"
#include "sys/process.hpp"
#include "sys/time.hpp"

#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace doas {

namespace {

constexpr const char* kTimestampDir = DOAS_PERSIST_DIR;
constexpr size_t kPathSize = 256;
constexpr size_t kProcStatSize = 1024;

bool clock_now(clockid_t clock, struct timespec* out) {
    return ::clock_gettime(clock, out) == 0;
}

// proc_info(): reads the tty number and the session leader start time from
// /proc/<pid>/stat.
int proc_info(pid_t pid, int* ttynr, unsigned long long* starttime) {
    std::array<char, 128> path{};
    std::array<char, kProcStatSize> buf{};

    const int n = std::snprintf(path.data(), path.size(), "/proc/%d/stat", pid);
    if (n < 0 || n >= static_cast<int>(path.size())) {
        return -1;
    }

    auto fd = sys::fs::open_file(path.data(), O_RDONLY | O_NOFOLLOW);
    if (!fd) {
        sys::warn_errno(std::format("failed to open: {}", path.data()));
        return -1;
    }

    char* const begin = buf.data();
    char* const limit = begin + (buf.size() - 1);
    char* p = begin;
    ssize_t got = 0;
    while ((got = ::read(fd->get(), p, static_cast<size_t>(limit - p))) != 0) {
        if (got == -1) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            sys::warn_errno(std::format("read: {}", path.data()));
            return -1;
        }
        p += got;
        if (p >= limit) {
            break;
        }
    }

    // error if it contains NULL bytes
    if (got != 0 || p == begin) {
        sys::warn_errno(std::format("NUL in: {}", path.data()));
        return -1;
    }
    if (std::memchr(begin, '\0', static_cast<size_t>(p - begin) - 1) != nullptr) {
        sys::warn_errno(std::format("NUL in: {}", path.data()));
        return -1;
    }

    *p = '\0';

    // Get the 7th field, 5 fields after the last ')', because the 5th field
    // 'comm' can include spaces and closing parentheses too.
    p = std::strrchr(begin, ')');
    if (p == nullptr) {
        return -1;
    }

    int field = 2;
    char* saveptr = nullptr;
    for (char* token = ::strtok_r(p, " ", &saveptr); token != nullptr;
         token = ::strtok_r(nullptr, " ", &saveptr)) {
        switch (field++) {
        case 7: {
            const sys::IntResult parsed = sys::parse_int(token, INT_MIN, INT_MAX);
            if (parsed.has_error()) {
                return -1;
            }
            *ttynr = static_cast<int>(parsed.value);
            break;
        }
        case 22: {
            const sys::U64Result parsed = sys::parse_u64(token);
            if (!parsed.ok) {
                return -1;
            }
            *starttime = parsed.value;
            return 0;
        }
        default:
            break;
        }
    }

    return -1;
}

int timestamp_path(char* buf, size_t len) {
    unsigned long long starttime = 0;
    int ttynr = 0;

    const pid_t ppid = sys::process::ppid();
    const auto sid = sys::process::session_id();
    if (!sid) {
        return -1;
    }
    if (proc_info(ppid, &ttynr, &starttime) == -1) {
        return -1;
    }
    const int n = std::snprintf(buf,
                                len,
                                "%s/%d-%d-%d-%llu-%d",
                                kTimestampDir,
                                static_cast<int>(ppid),
                                static_cast<int>(*sid),
                                ttynr,
                                starttime,
                                static_cast<int>(sys::process::uid()));
    if (n < 0 || n >= static_cast<int>(len)) {
        return -1;
    }
    return 0;
}

} // namespace

int timestamp_set(int fd, int secs) {
    struct timespec ts[2];
    const struct timespec timeout = {.tv_sec = secs, .tv_nsec = 0};

    if (!clock_now(CLOCK_BOOTTIME, &ts[0]) || !clock_now(CLOCK_REALTIME, &ts[1])) {
        return -1;
    }

    ts[0] = sys::ts_add(ts[0], timeout);
    ts[1] = sys::ts_add(ts[1], timeout);
    if (sys::fs::futimens_fd(fd, ts)) {
        return -1;
    }
    return 0;
}

// Returns 1 if the timestamp is valid, 0 if it is invalid.
int timestamp_check(int fd, int secs) {
    struct timespec ts[2];
    const struct timespec timeout = {.tv_sec = secs, .tv_nsec = 0};

    const sys::fs::StatResult st = sys::fs::fstat_fd(fd);
    if (st.error) {
        sys::die_errno(1, "fstat");
    }
    if (st.st.st_uid != 0 || st.st.st_gid != sys::process::gid() ||
        st.st.st_mode != (S_IFREG | 0000)) {
        sys::die(1, "timestamp uid, gid or mode wrong");
    }

    // this timestamp was created but never set, invalid but no error
    if (!sys::ts_set(st.st.st_atim) || !sys::ts_set(st.st.st_mtim)) {
        return 0;
    }

    if (!clock_now(CLOCK_BOOTTIME, &ts[0]) || !clock_now(CLOCK_REALTIME, &ts[1])) {
        sys::warn_errno("clock_gettime");
        return 0;
    }

    // check if timestamp is too old
    if (sys::ts_lt(st.st.st_atim, ts[0]) || sys::ts_lt(st.st.st_mtim, ts[1])) {
        return 0;
    }

    // check if timestamp is too far in the future
    ts[0] = sys::ts_add(ts[0], timeout);
    ts[1] = sys::ts_add(ts[1], timeout);
    if (sys::ts_gt(st.st.st_atim, ts[0]) || sys::ts_gt(st.st.st_mtim, ts[1])) {
        sys::warn("timestamp too far in the future");
        return 0;
    }

    return 1;
}

std::optional<sys::fs::Fd> timestamp_open(int* valid, int secs) {
    struct timespec zero[2] = {};
    std::array<char, kPathSize> path{};
    int serrno = 0;

    *valid = 0;

    const sys::fs::StatResult st = sys::fs::stat_follow(kTimestampDir);
    if (st.error) {
        if (st.error.value() != ENOENT) {
            return std::nullopt;
        }
        if (sys::fs::mkdir_path(kTimestampDir, 0700)) {
            return std::nullopt;
        }
    } else if (st.st.st_uid != 0 || st.st.st_mode != (S_IFDIR | 0700)) {
        return std::nullopt;
    }

    if (timestamp_path(path.data(), path.size()) == -1) {
        return std::nullopt;
    }

    auto fd = sys::fs::open_file(path.data(), O_RDONLY | O_NOFOLLOW);
    if (!fd) {
        if (errno != ENOENT) {
            sys::die_errno(1, std::format("open: {}", path.data()));
        }

        std::array<char, kPathSize> tmp{};
        const int n = std::snprintf(tmp.data(),
                                    tmp.size(),
                                    "%s/.tmp-%d",
                                    kTimestampDir,
                                    static_cast<int>(sys::process::pid()));
        if (n < 0 || n >= static_cast<int>(tmp.size())) {
            return std::nullopt;
        }

        auto created =
            sys::fs::open_file(tmp.data(), O_RDONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0000);
        if (!created) {
            return std::nullopt;
        }
        if (sys::fs::futimens_fd(created->get(), zero) ||
            sys::fs::rename_path(tmp.data(), path.data())) {
            serrno = errno;
            (void)sys::fs::unlink_path(tmp.data());
            errno = serrno;
            return std::nullopt;
        }
        return std::move(*created);
    }

    *valid = timestamp_check(fd->get(), secs);
    return std::move(*fd);
}

int timestamp_clear() {
    std::array<char, kPathSize> path{};

    if (timestamp_path(path.data(), path.size()) == -1) {
        return -1;
    }
    const std::error_code error = sys::fs::unlink_path(path.data());
    if (error && error.value() != ENOENT) {
        return -1;
    }
    return 0;
}

} // namespace doas

#endif // DOAS_PERSIST
