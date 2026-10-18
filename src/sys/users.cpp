#include "sys/users.hpp"

#include <cerrno>
#include <climits> // IWYU pragma: keep
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <unistd.h>
#include <vector>

namespace {

std::error_code make_error(int value) {
    return value == 0 ? std::error_code{} : std::error_code(value, std::generic_category());
}

sys::PasswdEntry copy_passwd(const struct passwd& pw) {
    sys::PasswdEntry entry;
    if (pw.pw_name != nullptr) {
        entry.name = pw.pw_name;
    }
    if (pw.pw_passwd != nullptr) {
        entry.passwd = pw.pw_passwd;
    }
    if (pw.pw_dir != nullptr) {
        entry.dir = pw.pw_dir;
    }
    if (pw.pw_shell != nullptr) {
        entry.shell = pw.pw_shell;
    }
    entry.uid = pw.pw_uid;
    entry.gid = pw.pw_gid;
    return entry;
}

// Grows the scratch buffer on ERANGE, exactly like mygetpwuid_r() from doas.c.
template <typename Lookup>
sys::PwResult lookup_pw(Lookup lookup) {
    sys::PwResult result;

    static long pwsz = 0;
    if (pwsz == 0) {
        pwsz = sysconf(_SC_GETPW_R_SIZE_MAX);
    }

    size_t buflen = pwsz > 0 ? static_cast<size_t>(pwsz) : 1024;
    std::vector<char> buf(buflen);
    struct passwd pwd{};
    struct passwd* entry = nullptr;

    int rv = 0;
    for (;;) {
        rv = lookup(&pwd, buf.data(), buf.size(), &entry);
        if (rv != ERANGE) {
            break;
        }
        const size_t newsz = buf.size() * 2;
        if (newsz < buf.size()) {
            break;
        }
        buf.resize(newsz);
    }

    if (rv != 0) {
        result.error = make_error(rv);
        return result;
    }
    if (entry != nullptr) {
        result.entry = copy_passwd(*entry);
    }
    return result;
}

} // namespace

namespace sys {

PwResult pw_by_uid(uid_t uid) {
    return lookup_pw([uid](struct passwd* pwd, char* buf, size_t len, struct passwd** out) {
        return getpwuid_r(uid, pwd, buf, len, out);
    });
}

PwResult pw_by_name(std::string_view name) {
    const std::string key(name);
    return lookup_pw([&key](struct passwd* pwd, char* buf, size_t len, struct passwd** out) {
        return getpwnam_r(key.c_str(), pwd, buf, len, out);
    });
}

GrResult gr_by_name(std::string_view name) {
    GrResult result;

    long grsz = sysconf(_SC_GETGR_R_SIZE_MAX);
    size_t buflen = grsz > 0 ? static_cast<size_t>(grsz) : 1024;
    std::vector<char> buf(buflen);
    struct group grp{};
    struct group* entry = nullptr;
    const std::string key(name);

    int rv = 0;
    for (;;) {
        rv = getgrnam_r(key.c_str(), &grp, buf.data(), buf.size(), &entry);
        if (rv != ERANGE) {
            break;
        }
        const size_t newsz = buf.size() * 2;
        if (newsz < buf.size()) {
            break;
        }
        buf.resize(newsz);
    }

    if (rv != 0) {
        result.error = make_error(rv);
        return result;
    }
    if (entry != nullptr) {
        GroupEntry found;
        if (entry->gr_name != nullptr) {
            found.name = entry->gr_name;
        }
        found.gid = entry->gr_gid;
        result.entry = std::move(found);
    }
    return result;
}

ShadowResult shadow_hash(std::string_view name) {
    ShadowResult result;

    long spsz = sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t buflen = spsz > 0 ? static_cast<size_t>(spsz) : 1024;
    std::vector<char> buf(buflen);
    struct spwd sp{};
    struct spwd* entry = nullptr;
    const std::string key(name);

    int rv = 0;
    for (;;) {
        rv = getspnam_r(key.c_str(), &sp, buf.data(), buf.size(), &entry);
        if (rv != ERANGE) {
            break;
        }
        const size_t newsz = buf.size() * 2;
        if (newsz < buf.size()) {
            break;
        }
        buf.resize(newsz);
    }

    if (rv != 0) {
        result.error = make_error(rv);
        return result;
    }
    if (entry != nullptr && entry->sp_pwdp != nullptr) {
        result.hash = std::string(entry->sp_pwdp);
    }
    return result;
}

GroupsResult supplementary_groups() {
    GroupsResult result;

    std::vector<gid_t> groups(static_cast<size_t>(NGROUPS_MAX));
    const int count = getgroups(static_cast<int>(groups.size()), groups.data());
    if (count == -1) {
        result.error = make_error(errno);
        return result;
    }
    groups.resize(static_cast<size_t>(count));
    result.groups = std::move(groups);
    return result;
}

} // namespace sys
