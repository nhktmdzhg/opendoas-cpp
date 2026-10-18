#ifndef DOAS_SYS_USERS_HPP
#define DOAS_SYS_USERS_HPP

#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
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

struct PwResult {
    std::error_code error;
    std::optional<PasswdEntry> entry;
};

struct GroupEntry {
    std::string name;
    gid_t gid = 0;
};

struct GrResult {
    std::error_code error;
    std::optional<GroupEntry> entry;
};

struct ShadowResult {
    std::error_code error;
    std::optional<std::string> hash;
};

struct GroupsResult {
    std::error_code error;
    std::vector<gid_t> groups;
};

// Replacements for mygetpwuid_r()/getpwnam(3)/getgrnam(3)/getspnam(3): the
// reentrant variants are used with the ERANGE growth loop from doas.c.
[[nodiscard]] PwResult pw_by_uid(uid_t uid);
[[nodiscard]] PwResult pw_by_name(std::string_view name);
[[nodiscard]] GrResult gr_by_name(std::string_view name);
[[nodiscard]] ShadowResult shadow_hash(std::string_view name);

// getgroups(NGROUPS_MAX, ...) with the same buffer size the original used.
[[nodiscard]] GroupsResult supplementary_groups();

} // namespace sys

#endif // DOAS_SYS_USERS_HPP
