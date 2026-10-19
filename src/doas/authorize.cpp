// authorize_command(): the rule matching and authentication half of doas.c main(),
// split out so that the doasedit applet can reuse it for its own privileged
// operations instead of running "doas cat"/"doas dd"/"doas stat" as subprocesses.
#include "doas/authorize.hpp"

#include "doas/auth.hpp"
#include "sys/error.hpp"
#include "sys/fs.hpp"
#include "sys/process.hpp"
#include "sys/strings.hpp"
#include "sys/users.hpp"

#include <climits>
#include <format>
#include <string>
#include <syslog.h>
#include <utility>
#include <vector>

namespace doas {

Rule authorize_command(std::string_view cmd,
                       std::span<const std::string> args,
                       uid_t target,
                       AuthMode mode,
                       bool interactive,
                       std::string_view config) {
    const uid_t uid = sys::process::uid();

    auto self_pw = sys::pw_by_uid(uid);
    if (!self_pw) {
        if (self_pw.error().is(sys::Err::kNotFound)) {
            sys::die(1, "no passwd entry for self");
        }
        sys::die(1, self_pw.error(), "getpwuid_r failed");
    }

    auto groups_result = sys::supplementary_groups();
    if (!groups_result) {
        sys::die(1, groups_result.error(), "can't get groups");
    }
    std::vector<gid_t> groups = std::move(*groups_result);
    groups.push_back(sys::process::gid());

    if (sys::process::euid() != 0) {
        sys::die(1, "not installed setuid");
    }

    parse_config(config.empty() ? config_path() : config, true);

    // cmdline is used only for logging, no need to abort on truncate
    sys::BoundedString cmdline(LINE_MAX);
    cmdline.assign(cmd);
    for (const std::string& arg : args) {
        if (!cmdline.append(" ")) {
            break;
        }
        if (!cmdline.append(arg)) {
            break;
        }
    }

    const MatchContext ctx{uid, groups, target};
    const PermitResult result = permit(ctx, cmd, args);
    if (!result.allowed || !result.rule) {
        sys::log(LOG_AUTHPRIV | LOG_NOTICE,
                 std::format("command not permitted for {}: {}", self_pw->name, cmdline.view()));
        sys::die(1, sys::fail_errno(EPERM));
    }
    const Rule& rule = *result.rule;

    auto target_pw = sys::pw_by_uid(target);
    if (!target_pw) {
        if (target_pw.error().is(sys::Err::kNotFound)) {
            sys::die(1, "no passwd entry for target");
        }
        sys::die(1, target_pw.error(), "getpwuid_r failed");
    }

    if (mode != AuthMode::kAlreadyAuthenticated) {
#if defined(DOAS_HAVE_SHADOW)
        if ((rule.options & kNopass) == 0) {
            if (!interactive) {
                sys::die(1, "Authentication required");
            }
            shadow_auth(self_pw->name, (rule.options & kPersist) != 0);
        }
#endif

#if defined(DOAS_HAVE_PAM)
        pamauth(target_pw->name,
                self_pw->name,
                interactive,
                (rule.options & kNopass) != 0,
                (rule.options & kPersist) != 0);
#endif
    }

    if ((rule.options & kNolog) == 0) {
        const std::optional<std::string> cwd = sys::fs::current_dir();
        sys::log(LOG_AUTHPRIV | LOG_INFO,
                 std::format("{} ran command {} as {} from {}",
                             self_pw->name,
                             cmdline.view(),
                             target_pw->name,
                             cwd.value_or("(failed)")));
    }

    return rule;
}

} // namespace doas
