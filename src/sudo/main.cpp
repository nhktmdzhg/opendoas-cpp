#include "doas/doas.hpp"
#include "sudo/sudo.hpp"
#include "sys/error.hpp"
#include "sys/io.hpp"
#include "sys/process.hpp"
#include "sys/users.hpp"
#include "version.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sudo {

namespace {

constexpr std::string_view kHelpText =
    "sudo (opendoas-cpp) - translate a sudo command line into doas arguments\n"
    "\n"
    "Usage: sudo [-h|--help] [-v|--version] [sudo options] [command [args]...]\n"
    "\n"
    "Options:\n"
    "  -h, --help     Print help\n"
    "  -v, --version  Print version\n"
    "\n"
    "Every other sudo option is translated to its doas equivalent or ignored; what may\n"
    "run is decided by doas.conf(5).\n";

} // namespace

int main(int argc, char* const argv[]) {
    // Help, version and sudo -e are answered before anything else: they must not
    // need a doas binary and must not print a trace.
    const TranslateResult translated = translate(argc, argv);
    switch (translated.request) {
    case Request::kHelp:
        sys::out(kHelpText);
        return 0;
    case Request::kVersion:
        sys::out(meta::applet_version("sudo"));
        return 0;
    case Request::kExit:
        return 0;
    case Request::kNone:
        break;
    }

    // Kept for parity with the original (which reports a missing doas binary and
    // exits with 255); the resolved path is only used for the trace output.
    const std::optional<std::string> doas_prog = sys::process::which("doas");
    if (!doas_prog) {
        sys::err_out("doas couldn't be found on your system.\n");
        return 255;
    }

    {
        // Setup environment
        const auto pw = sys::pw_by_uid(sys::process::uid());
        if (!pw) {
            // The original dereferenced the NULL passwd entry here.
            sys::die(1,
                     pw.error().is(sys::Err::kNotFound) ? sys::fail_errno(0) : pw.error(),
                     "no passwd entry for self");
        }
        // The original ignored setenv(3) failures here as well.
        (void)sys::process::set_env("SUDO_USER", pw->name);
        (void)sys::process::set_env("SUDO_UID", std::to_string(pw->uid));
        (void)sys::process::set_env("SUDO_GID", std::to_string(pw->gid));
    }

    std::vector<std::string> args;
    args.push_back(*doas_prog);
    for (const std::string& arg : translated.args) {
        args.push_back(arg);
    }

    // The doas applet now runs in this very process (the original exec'd a doas
    // binary): raise the effective uid back to root from the saved set user id
    // and switch the diagnostics to the doas program name. When the binary is not
    // installed setuid the raise fails and the doas applet reports it itself, so
    // this is best effort.
    sys::set_progname("doas");
    (void)sys::process::set_res_uid(sys::process::kKeepId, 0, sys::process::kKeepId);

    std::vector<char*> exec_argv;
    exec_argv.reserve(args.size() + 1);
    for (std::string& arg : args) {
        exec_argv.push_back(arg.data());
    }
    exec_argv.push_back(nullptr);

    doas::replace_main(static_cast<int>(args.size()), exec_argv.data());
}

} // namespace sudo
