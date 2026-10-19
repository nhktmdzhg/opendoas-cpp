#include "doas/config.hpp"

#include "sys/error.hpp"
#include "sys/fs.hpp"
#include "sys/io.hpp"
#include "sys/numbers.hpp"
#include "sys/users.hpp"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <sys/stat.h>

namespace doas {

std::vector<Rule> g_rules;
int g_parse_errors = 0;

namespace {

// UID_MAX/GID_MAX are configure time constants in the original.
constexpr uid_t kUidMax = DOAS_UID_MAX;
constexpr gid_t kGidMax = DOAS_GID_MAX;

} // namespace

std::optional<uid_t> parse_uid(std::string_view text) {
    // parseuid(): a passwd entry wins over a number.
    if (const auto pw = sys::pw_by_name(text)) {
        if (pw->uid == kUidMax) {
            return std::nullopt;
        }
        return pw->uid;
    }
    const auto num = sys::parse_int(text, 0, static_cast<long long>(kUidMax) - 1);
    if (!num) {
        return std::nullopt;
    }
    return static_cast<uid_t>(*num);
}

std::optional<gid_t> parse_gid(std::string_view text) {
    if (const auto gr = sys::gr_by_name(text)) {
        if (gr->gid == kGidMax) {
            return std::nullopt;
        }
        return gr->gid;
    }
    const auto num = sys::parse_int(text, 0, static_cast<long long>(kGidMax) - 1);
    if (!num) {
        return std::nullopt;
    }
    return static_cast<gid_t>(*num);
}

bool match_rule(const MatchContext& ctx,
                const Rule& rule,
                std::string_view cmd,
                std::span<const std::string> cmdargs) {
    if (!rule.ident.empty() && rule.ident[0] == ':') {
        const std::optional<gid_t> rgid = parse_gid(std::string_view(rule.ident).substr(1));
        if (!rgid) {
            return false;
        }
        bool found = false;
        for (const gid_t group : ctx.groups) {
            if (*rgid == group) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    } else {
        const std::optional<uid_t> uid = parse_uid(rule.ident);
        if (!uid || *uid != ctx.uid) {
            return false;
        }
    }
    if (rule.target) {
        const std::optional<uid_t> target = parse_uid(*rule.target);
        if (!target || *target != ctx.target) {
            return false;
        }
    }
    if (rule.cmd) {
        if (*rule.cmd != cmd) {
            return false;
        }
        if (rule.cmdargs) {
            // if arguments were given, they should match explicitly
            if (rule.cmdargs->size() != cmdargs.size()) {
                return false;
            }
            for (size_t i = 0; i < cmdargs.size(); i++) {
                if ((*rule.cmdargs)[i] != cmdargs[i]) {
                    return false;
                }
            }
        }
    }
    return true;
}

PermitResult
permit(const MatchContext& ctx, std::string_view cmd, std::span<const std::string> cmdargs) {
    PermitResult result;
    for (const Rule& rule : g_rules) {
        if (match_rule(ctx, rule, cmd, cmdargs)) {
            result.rule = rule;
        }
    }
    if (!result.rule) {
        return result;
    }
    result.allowed = result.rule->action == kPermit;
    return result;
}

namespace {

// Storage for report_config_error(); empty when no collector is active.
std::optional<std::string> g_collected_diagnostics;

// Owns the diagnostics collector for as long as it lives, so that
// validate_config() cannot leave it armed on any return path. Constructing it also
// resets the rule set and the error counter, which makes repeated validations
// independent.
class CollectGuard {
  public:
    CollectGuard() {
        g_rules.clear();
        g_parse_errors = 0;
        g_collected_diagnostics.emplace();
    }
    ~CollectGuard() {
        g_collected_diagnostics.reset();
    }
    CollectGuard(const CollectGuard&) = delete;
    CollectGuard(CollectGuard&&) = delete;
    CollectGuard& operator=(const CollectGuard&) = delete;
    CollectGuard& operator=(CollectGuard&&) = delete;
};

// The diagnostics collected so far; empty when no guard is active.
[[nodiscard]] std::string collected_diagnostics() {
    return g_collected_diagnostics.value_or(std::string{});
}

} // namespace

std::string_view config_path() {
    return DOAS_CONF;
}

void report_config_error(std::string_view text) {
    if (g_collected_diagnostics) {
        *g_collected_diagnostics += text;
        return;
    }
    sys::err_out(text);
}

sys::Result<std::string> validate_config(std::string_view filename) {
    std::optional<sys::FileHandle> file = sys::FileHandle::open_read(filename);
    if (!file) {
        return std::unexpected(sys::fail_errno(errno));
    }

    const CollectGuard guard;
    run_parser(file->raw());
    file.reset();
    return collected_diagnostics();
}

void parse_config(std::string_view filename, bool checkperms) {
    std::optional<sys::FileHandle> file = sys::FileHandle::open_read(filename);
    if (!file) {
        // The original printed the errno of the failed open here.
        if (checkperms) {
            sys::die(1, sys::fail_errno(errno), std::format("doas is not enabled, {}", filename));
        }
        sys::die(1, sys::fail_errno(errno), std::format("could not open config file {}", filename));
    }

    if (checkperms) {
        const auto sb = sys::fs::fstat_fd(file->fd());
        if (!sb) {
            sys::die(1, sb.error(), std::format("fstat(\"{}\")", filename));
        }
        if ((sb->st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            sys::die(1, std::format("{} is writable by group or other", filename));
        }
        if (sb->st_uid != 0) {
            sys::die(1, std::format("{} is not owned by root", filename));
        }
    }

    run_parser(file->raw());
    file.reset();
    if (g_parse_errors != 0) {
        std::exit(1);
    }
}

} // namespace doas
