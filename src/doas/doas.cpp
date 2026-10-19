// Port of doas.c main(), trial for trial: the same option parsing, the same
// order of config parsing / permission check / authentication / privilege drop,
// the same diagnostics and the same exit codes.
#include "doas/doas.hpp"

#include "doas/authorize.hpp"
#include "doas/config.hpp"
#include "doas/env.hpp"
#include "doas/exec.hpp"
#include "doas/timestamp.hpp"
#include "sys/error.hpp"
#include "sys/io.hpp"
#include "sys/process.hpp"
#include "sys/users.hpp"

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
            // timestamp_clear() returned 0/-1; a failure is exit status 1.
            std::exit(timestamp_clear() ? 0 : 1);
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

    const char* cmd = cmd_argv[0];
    const std::vector<std::string> cmd_args = collect_args(cmd_argc, cmd_argv, 1);

    const Rule rule = authorize_command(cmd, cmd_args, target, AuthMode::kAuthenticate, !nflag);

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
