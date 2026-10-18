#ifndef DOAS_SYS_USERS_HPP
#define DOAS_SYS_USERS_HPP

#include "sys/error.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace sys {

// Snapshot of a passwd entry. Strings are copied out so that no caller keeps a
// pointer into libc's static storage (crypt(3), called right after getspnam(3),
// made that aliasing fragile in the original code).
struct PasswdEntry {
    std::string name;
    std::string passwd;
    std::string dir;
    std::string shell;
    uid_t uid = 0;
    gid_t gid = 0;
};

struct GroupEntry {
    std::string name;
    gid_t gid = 0;
};

// Replacements for mygetpwuid_r()/getpwnam(3)/getgrnam(3)/getspnam(3): the
// reentrant variants are used with the ERANGE growth loop from doas.c. A missing
// entry is Err::kNotFound; a failing lookup carries the errno.
[[nodiscard]] Result<PasswdEntry> pw_by_uid(uid_t uid);
[[nodiscard]] Result<PasswdEntry> pw_by_name(std::string_view name);
[[nodiscard]] Result<GroupEntry> gr_by_name(std::string_view name);
[[nodiscard]] Result<std::string> shadow_hash(std::string_view name);

// crypt(3): returns a copy of the hash, or nothing when libc refuses to hash.
// The static buffer crypt(3) hands out never escapes this function.
[[nodiscard]] std::optional<std::string> crypt_hash(std::string_view password,
                                                    std::string_view setting);

// getgroups(NGROUPS_MAX, ...) with the same buffer size the original used.
[[nodiscard]] Result<std::vector<gid_t>> supplementary_groups();

} // namespace sys

#endif // DOAS_SYS_USERS_HPP
