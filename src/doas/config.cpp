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
    const sys::PwResult pw = sys::pw_by_name(text);
    if (pw.entry) {
        const uid_t uid = pw.entry->uid;
        if (uid == kUidMax) {
            return std::nullopt;
        }
        return uid;
    }
    const sys::IntResult num = sys::parse_int(text, 0, static_cast<long long>(kUidMax) - 1);
    if (num.has_error()) {
        return std::nullopt;
    }
    return static_cast<uid_t>(num.value);
}

std::optional<gid_t> parse_gid(std::string_view text) {
    const sys::GrResult gr = sys::gr_by_name(text);
    if (gr.entry) {
        const gid_t gid = gr.entry->gid;
        if (gid == kGidMax) {
            return std::nullopt;
        }
        return gid;
    }
    const sys::IntResult num = sys::parse_int(text, 0, static_cast<long long>(kGidMax) - 1);
    if (num.has_error()) {
        return std::nullopt;
    }
    return static_cast<gid_t>(num.value);
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

void parse_config(std::string_view filename, bool checkperms) {
    std::optional<sys::FileHandle> file = sys::FileHandle::open_read(filename);
    if (!file) {
        if (checkperms) {
            sys::die_errno(1, std::format("doas is not enabled, {}", filename));
        }
        sys::die_errno(1, std::format("could not open config file {}", filename));
    }

    if (checkperms) {
        const sys::fs::StatResult sb = sys::fs::fstat_fd(file->fd());
        if (sb.error) {
            sys::die_errno(1, std::format("fstat(\"{}\")", filename));
        }
        if ((sb.st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            sys::die(1, std::format("{} is writable by group or other", filename));
        }
        if (sb.st.st_uid != 0) {
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
