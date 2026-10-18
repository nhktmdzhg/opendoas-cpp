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
#include <cstring>
#include <fcntl.h>
#include <format>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace doas {

namespace {

constexpr const char* kTimestampDir = DOAS_PERSIST_DIR;
constexpr size_t kProcStatSize = 1024;

struct ProcInfo {
    int tty_nr = 0;
    unsigned long long start_time = 0;
};

bool clock_now(clockid_t clock, struct timespec* out) {
    return ::clock_gettime(clock, out) == 0;
}

// proc_info(): reads the tty number and the session leader start time from
// /proc/<pid>/stat.
sys::Result<ProcInfo> proc_info(pid_t pid) {
    const std::string path = std::format("/proc/{}/stat", pid);

    auto fd = sys::fs::open_file(path, O_RDONLY | O_NOFOLLOW);
    if (!fd) {
        sys::warn(fd.error(), std::format("failed to open: {}", path));
        return std::unexpected(fd.error());
    }

    std::array<char, kProcStatSize> buf{};
    char* const begin = buf.data();
    char* const limit = begin + (buf.size() - 1);
    char* p = begin;
    size_t got = 0;
    for (;;) {
        const auto read = sys::fs::read_fd(fd->get(), {p, static_cast<size_t>(limit - p)});
        if (!read) {
            if (read.error().code == std::errc::resource_unavailable_try_again ||
                read.error().code == std::errc::interrupted) {
                continue;
            }
            sys::warn(read.error(), std::format("read: {}", path));
            return std::unexpected(read.error());
        }
        got = *read;
        if (got == 0) {
            break;
        }
        p += got;
        if (p >= limit) {
            break;
        }
    }

    // error if it contains NULL bytes
    if (got != 0 || p == begin) {
        sys::warn(sys::fail_errno(0), std::format("NUL in: {}", path));
        return std::unexpected(sys::fail_errno(0));
    }
    if (std::memchr(begin, '\0', static_cast<size_t>(p - begin) - 1) != nullptr) {
        sys::warn(sys::fail_errno(0), std::format("NUL in: {}", path));
        return std::unexpected(sys::fail_errno(0));
    }

    *p = '\0';

    // Get the 7th field, 5 fields after the last ')', because the 5th field
    // 'comm' can include spaces and closing parentheses too.
    p = std::strrchr(begin, ')');
    if (p == nullptr) {
        return std::unexpected(sys::fail(sys::Err::kInvalidNumber));
    }

    ProcInfo info;
    int field = 2;
    char* saveptr = nullptr;
    for (char* token = ::strtok_r(p, " ", &saveptr); token != nullptr;
         token = ::strtok_r(nullptr, " ", &saveptr)) {
        switch (field++) {
        case 7: {
            const auto parsed = sys::parse_int(token, INT_MIN, INT_MAX);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            info.tty_nr = static_cast<int>(*parsed);
            break;
        }
        case 22: {
            const auto parsed = sys::parse_u64(token);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            info.start_time = *parsed;
            return info;
        }
        default:
            break;
        }
    }

    return std::unexpected(sys::fail(sys::Err::kInvalidNumber));
}

sys::Result<std::string> timestamp_path() {
    const pid_t ppid = sys::process::ppid();
    const auto sid = sys::process::session_id();
    if (!sid) {
        return std::unexpected(sid.error());
    }
    const auto info = proc_info(ppid);
    if (!info) {
        return std::unexpected(info.error());
    }
    return std::format("{}/{}-{}-{}-{}-{}",
                       kTimestampDir,
                       static_cast<int>(ppid),
                       static_cast<int>(*sid),
                       info->tty_nr,
                       info->start_time,
                       static_cast<int>(sys::process::uid()));
}

} // namespace

sys::Status timestamp_set(const sys::fs::Fd& fd, int secs) {
    struct timespec ts[2];
    const struct timespec timeout = {.tv_sec = secs, .tv_nsec = 0};

    if (!clock_now(CLOCK_BOOTTIME, &ts[0]) || !clock_now(CLOCK_REALTIME, &ts[1])) {
        return std::unexpected(sys::fail_errno(errno));
    }

    ts[0] = sys::ts_add(ts[0], timeout);
    ts[1] = sys::ts_add(ts[1], timeout);
    return sys::fs::futimens_fd(fd.get(), ts);
}

namespace {

// Returns true if the timestamp is valid. Fatal inconsistencies exit through
// sys::die, exactly like the errx() calls of the original.
bool timestamp_check(int fd, int secs) {
    struct timespec ts[2];
    const struct timespec timeout = {.tv_sec = secs, .tv_nsec = 0};

    const auto st = sys::fs::fstat_fd(fd);
    if (!st) {
        sys::die(1, st.error(), "fstat");
    }
    if (st->st_uid != 0 || st->st_gid != sys::process::gid() || st->st_mode != (S_IFREG | 0000)) {
        sys::die(1, "timestamp uid, gid or mode wrong");
    }

    // this timestamp was created but never set, invalid but no error
    if (!sys::ts_set(st->st_atim) || !sys::ts_set(st->st_mtim)) {
        return false;
    }

    if (!clock_now(CLOCK_BOOTTIME, &ts[0]) || !clock_now(CLOCK_REALTIME, &ts[1])) {
        sys::warn(sys::fail_errno(errno), "clock_gettime");
        return false;
    }

    // check if timestamp is too old
    if (sys::ts_lt(st->st_atim, ts[0]) || sys::ts_lt(st->st_mtim, ts[1])) {
        return false;
    }

    // check if timestamp is too far in the future
    ts[0] = sys::ts_add(ts[0], timeout);
    ts[1] = sys::ts_add(ts[1], timeout);
    if (sys::ts_gt(st->st_atim, ts[0]) || sys::ts_gt(st->st_mtim, ts[1])) {
        sys::warn("timestamp too far in the future");
        return false;
    }

    return true;
}

} // namespace

sys::Result<OpenedTimestamp> timestamp_open(int secs) {
    struct timespec zero[2] = {};

    const auto st = sys::fs::stat_follow(kTimestampDir);
    if (!st) {
        if (!(st.error().code == std::errc::no_such_file_or_directory)) {
            return std::unexpected(st.error());
        }
        const sys::Status made = sys::fs::mkdir_path(kTimestampDir, 0700);
        if (!made) {
            return std::unexpected(made.error());
        }
    } else if (st->st_uid != 0 || st->st_mode != (S_IFDIR | 0700)) {
        // The original returned -1 without a message here; EACCES keeps that
        // shape without inventing a new diagnostic.
        return std::unexpected(sys::fail_errno(EACCES));
    }

    auto path = timestamp_path();
    if (!path) {
        return std::unexpected(path.error());
    }

    auto fd = sys::fs::open_file(*path, O_RDONLY | O_NOFOLLOW);
    if (!fd) {
        if (!(fd.error().code == std::errc::no_such_file_or_directory)) {
            sys::die(1, fd.error(), std::format("open: {}", *path));
        }

        const std::string tmp =
            std::format("{}/.tmp-{}", kTimestampDir, static_cast<int>(sys::process::pid()));
        auto created = sys::fs::open_file(tmp, O_RDONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0000);
        if (!created) {
            return std::unexpected(created.error());
        }
        const sys::Status stamped = sys::fs::futimens_fd(created->get(), zero);
        if (!stamped) {
            (void)sys::fs::unlink_path(tmp);
            return std::unexpected(stamped.error());
        }
        const sys::Status renamed = sys::fs::rename_path(tmp, *path);
        if (!renamed) {
            (void)sys::fs::unlink_path(tmp);
            return std::unexpected(renamed.error());
        }
        return OpenedTimestamp{std::move(*created), false};
    }

    const bool valid = timestamp_check(fd->get(), secs);
    return OpenedTimestamp{std::move(*fd), valid};
}

sys::Status timestamp_clear() {
    auto path = timestamp_path();
    if (!path) {
        return std::unexpected(path.error());
    }
    const sys::Status unlinked = sys::fs::unlink_path(*path);
    if (!unlinked && !(unlinked.error().code == std::errc::no_such_file_or_directory)) {
        return unlinked;
    }
    return {};
}

} // namespace doas

#endif // DOAS_PERSIST
