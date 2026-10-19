#include "sys/users.hpp"

#include "sys/secret.hpp"

#include <cerrno>
#include <climits>
#include <crypt.h>
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <unistd.h>
#include <vector>

namespace {

sys::PasswdEntry copy_passwd(const struct passwd& user) {
    sys::PasswdEntry entry;
    if (user.pw_name != nullptr) {
        entry.name = user.pw_name;
    }
    if (user.pw_passwd != nullptr) {
        entry.passwd = user.pw_passwd;
    }
    if (user.pw_dir != nullptr) {
        entry.dir = user.pw_dir;
    }
    if (user.pw_shell != nullptr) {
        entry.shell = user.pw_shell;
    }
    entry.uid = user.pw_uid;
    entry.gid = user.pw_gid;
    return entry;
}

// Grows the scratch buffer on ERANGE, exactly like mygetpwuid_r() from doas.c.
template <typename Lookup>
sys::Result<sys::PasswdEntry> lookup_pw(Lookup lookup) {
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
        return std::unexpected(sys::fail_errno(rv));
    }
    if (entry == nullptr) {
        return std::unexpected(sys::fail(sys::Err::kNotFound));
    }
    return copy_passwd(*entry);
}

} // namespace

namespace sys {

Result<PasswdEntry> pw_by_uid(uid_t uid) {
    return lookup_pw([uid](struct passwd* pwd, char* buf, size_t len, struct passwd** out) {
        return getpwuid_r(uid, pwd, buf, len, out);
    });
}

Result<PasswdEntry> pw_by_name(std::string_view name) {
    const std::string key(name);
    return lookup_pw([&key](struct passwd* pwd, char* buf, size_t len, struct passwd** out) {
        return getpwnam_r(key.c_str(), pwd, buf, len, out);
    });
}

Result<GroupEntry> gr_by_name(std::string_view name) {
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
        return std::unexpected(fail_errno(rv));
    }
    if (entry == nullptr) {
        return std::unexpected(fail(Err::kNotFound));
    }
    GroupEntry found;
    if (entry->gr_name != nullptr) {
        found.name = entry->gr_name;
    }
    found.gid = entry->gr_gid;
    return found;
}

Result<std::string> shadow_hash(std::string_view name) {
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
        return std::unexpected(fail_errno(rv));
    }
    if (entry == nullptr || entry->sp_pwdp == nullptr) {
        return std::unexpected(fail(Err::kNotFound));
    }
    return std::string(entry->sp_pwdp);
}

std::optional<std::string> crypt_hash(std::string_view password, std::string_view setting) {
    // crypt(3) needs NUL terminated strings. Both copies live in wiped buffers;
    // Secret zero initialises, so the terminator is already in place.
    sys::Secret pass(password.size() + 1);
    sys::Secret salt(setting.size() + 1);
    if (!password.empty()) {
        std::memcpy(pass.data(), password.data(), password.size());
    }
    if (!setting.empty()) {
        std::memcpy(salt.data(), setting.data(), setting.size());
    }
    char* hashed = ::crypt(pass.data(), salt.data());
    if (hashed == nullptr) {
        return std::nullopt;
    }
    return std::string(hashed);
}

Result<std::vector<gid_t>> supplementary_groups() {
    std::vector<gid_t> groups(static_cast<size_t>(NGROUPS_MAX));
    const int count = getgroups(static_cast<int>(groups.size()), groups.data());
    if (count == -1) {
        return std::unexpected(fail_errno(errno));
    }
    groups.resize(static_cast<size_t>(count));
    return groups;
}

} // namespace sys
