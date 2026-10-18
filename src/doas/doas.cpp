/* $OpenBSD: doas.c,v 1.52 2016/04/28 04:48:56 tedu Exp $ */
/*
 * Copyright (c) 2015 Ted Unangst <tedu@openbsd.org>
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

// Port of doas.c main(), trial for trial: the same option parsing, the same
// order of config parsing / permission check / authentication / privilege drop,
// the same diagnostics and the same exit codes.
#include "doas/doas.hpp"

#include "doas/auth.hpp"
#include "doas/config.hpp"
#include "doas/env.hpp"
#include "doas/exec.hpp"
#include "doas/timestamp.hpp"
#include "sys/error.hpp"
#include "sys/fs.hpp"
#include "sys/io.hpp"
#include "sys/process.hpp"
#include "sys/strings.hpp"
#include "sys/users.hpp"

#include <cerrno>
#include <climits> // IWYU pragma: keep
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <syslog.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#if !defined(DOAS_CONF)
#define DOAS_CONF "/etc/doas.conf"
#endif

namespace doas {

namespace {

constexpr std::string_view kSafePath = "/bin:/sbin:/usr/bin:/usr/sbin:"
                                       "/usr/local/bin:/usr/local/sbin";

std::vector<std::string> collect_args(int argc, char* const argv[], int first) {
    std::vector<std::string> out;
    if (argc > first) {
        out.reserve(static_cast<size_t>(argc - first));
    }
    for (int i = first; i < argc; i++) {
        out.emplace_back(argv[i]);
    }
    return out;
}

// checkconfig() from doas.c: parses the config and reports the matching result,
// never returning.
[[noreturn]] void check_config(std::string_view confpath,
                               int argc,
                               char* const argv[],
                               uid_t uid,
                               std::span<const gid_t> groups,
                               uid_t target) {
    if (sys::Status dropped = sys::process::set_res_uid(uid, uid, uid); !dropped) {
        sys::die(1, dropped.error(), "setresuid");
    }

    parse_config(confpath, false);
    if (argc == 0) {
        std::exit(0);
    }

    const MatchContext ctx{uid, groups, target};
    const PermitResult result = permit(ctx, argv[0], collect_args(argc, argv, 1));
    if (result.allowed && result.rule) {
        std::string text = "permit";
        if ((result.rule->options & kNopass) != 0) {
            text += " nopass";
        }
        text += "\n";
        sys::out(text);
        std::exit(0);
    }
    sys::out("deny\n");
    std::exit(1);
}

} // namespace

void usage() {
    sys::err_out("usage: doas [-Lns] [-C config] [-u user] command [args]\n");
    std::exit(1);
}

void replace_main(int argc, char* const argv[]) {
    sys::set_progname("doas");

    sys::process::close_from(STDERR_FILENO + 1);

    const uid_t uid = sys::process::uid();

    const char* confpath = nullptr;
    uid_t target = 0;
    bool sflag = false;
    bool nflag = false;

    int ch = 0;
    while ((ch = getopt(argc, argv, "+C:Lnsu:")) != -1) {
        switch (ch) {
        case 'C':
            confpath = optarg;
            break;
        case 'L':
#if defined(DOAS_PERSIST)
            std::exit(timestamp_clear() == -1);
#else
            std::exit(0);
#endif
        case 'u': {
            const std::optional<uid_t> parsed = parse_uid(optarg);
            if (!parsed) {
                sys::die(1, "unknown user");
            }
            target = *parsed;
            break;
        }
        case 'n':
            nflag = true;
            break;
        case 's':
            sflag = true;
            break;
        default:
            usage();
        }
    }

    char* const* cmd_argv = argv + optind;
    int cmd_argc = argc - optind;

    if (confpath != nullptr) {
        if (sflag) {
            usage();
        }
    } else if ((!sflag && cmd_argc == 0) || (sflag && cmd_argc != 0)) {
        usage();
    }

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

    // -s replaces the command with the shell from $SHELL or passwd(5).
    std::vector<char*> shell_argv;
    std::string shell;
    if (sflag) {
        const std::optional<std::string> env_shell = sys::process::get_env("SHELL");
        shell = (!env_shell || env_shell->empty()) ? self_pw->shell : *env_shell;
        shell_argv.push_back(shell.data());
        shell_argv.push_back(nullptr);
        cmd_argv = shell_argv.data();
        cmd_argc = 1;
    }

    if (confpath != nullptr) {
        check_config(confpath, cmd_argc, cmd_argv, uid, groups, target);
        std::exit(1); // fail safe
    }

    if (sys::process::euid() != 0) {
        sys::die(1, "not installed setuid");
    }

    parse_config(DOAS_CONF, true);

    // cmdline is used only for logging, no need to abort on truncate
    sys::BoundedString cmdline(LINE_MAX);
    cmdline.assign(cmd_argv[0]);
    for (int i = 1; i < cmd_argc; i++) {
        if (!cmdline.append(" ")) {
            break;
        }
        if (!cmdline.append(cmd_argv[i])) {
            break;
        }
    }

    const char* cmd = cmd_argv[0];
    const MatchContext ctx{uid, groups, target};
    const std::vector<std::string> cmd_args = collect_args(cmd_argc, cmd_argv, 1);
    const PermitResult result = permit(ctx, cmd, cmd_args);
    if (!result.allowed || !result.rule) {
        sys::log(LOG_AUTHPRIV | LOG_NOTICE,
                 std::format("command not permitted for {}: {}", self_pw->name, cmdline.view()));
        sys::die(1, sys::fail_errno(EPERM));
    }
    const Rule& rule = *result.rule;

#if defined(DOAS_HAVE_SHADOW)
    if ((rule.options & kNopass) == 0) {
        if (nflag) {
            sys::die(1, "Authentication required");
        }
        shadow_auth(self_pw->name, (rule.options & kPersist) != 0);
    }
#endif

    const std::string formerpath = sys::process::get_env("PATH").value_or("");

    if (rule.cmd) {
        if (sys::Status set = sys::process::set_env("PATH", kSafePath); !set) {
            sys::die(1, set.error(), std::format("failed to set PATH '{}'", kSafePath));
        }
    }

    auto target_pw = sys::pw_by_uid(target);
    if (!target_pw) {
        if (target_pw.error().is(sys::Err::kNotFound)) {
            sys::die(1, "no passwd entry for target");
        }
        sys::die(1, target_pw.error(), "getpwuid_r failed");
    }

#if defined(DOAS_HAVE_PAM)
    pamauth(target_pw->name,
            self_pw->name,
            !nflag,
            (rule.options & kNopass) != 0,
            (rule.options & kPersist) != 0);
#endif

    if (sys::Status gid = sys::process::set_res_gid(target_pw->gid, target_pw->gid, target_pw->gid);
        !gid) {
        sys::die(1, gid.error(), "setresgid");
    }
    if (sys::Status groups = sys::process::init_groups(target_pw->name, target_pw->gid); !groups) {
        sys::die(1, groups.error(), "initgroups");
    }
    if (sys::Status uid_set = sys::process::set_res_uid(target, target, target); !uid_set) {
        sys::die(1, uid_set.error(), "setresuid");
    }
    if (sys::Status set = sys::process::set_env("PATH", kSafePath); !set) {
        sys::die(1, set.error(), std::format("failed to set PATH '{}'", kSafePath));
    }

    const std::optional<std::string> cwd = sys::fs::current_dir();
    const std::string cwd_text = cwd.value_or("(failed)");

    if ((rule.options & kNolog) == 0) {
        sys::log(LOG_AUTHPRIV | LOG_INFO,
                 std::format("{} ran command {} as {} from {}",
                             self_pw->name,
                             cmdline.view(),
                             target_pw->name,
                             cwd_text));
    }

    std::vector<std::string> env_entries = prepenv(rule, *self_pw, *target_pw, formerpath);

    // setusercontext set path for the next process, so reset it for us
    if (rule.cmd) {
        if (sys::Status set = sys::process::set_env("PATH", kSafePath); !set) {
            sys::die(1, set.error(), std::format("failed to set PATH '{}'", kSafePath));
        }
    } else {
        if (sys::Status set = sys::process::set_env("PATH", formerpath); !set) {
            sys::die(1, set.error(), std::format("failed to set PATH '{}'", formerpath));
        }
    }

    std::vector<char*> envp;
    envp.reserve(env_entries.size() + 1);
    for (std::string& entry : env_entries) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    std::vector<char*> exec_argv;
    exec_argv.reserve(static_cast<size_t>(cmd_argc) + 1);
    for (int i = 0; i < cmd_argc; i++) {
        exec_argv.push_back(cmd_argv[i]);
    }
    exec_argv.push_back(nullptr);

    const sys::Error exec_error = execvpe(cmd, exec_argv, envp);
    if (exec_error.code == std::errc::no_such_file_or_directory) {
        sys::die(1, std::format("{}: command not found", cmd));
    }
    sys::die(1, exec_error, cmd);
}

} // namespace doas
