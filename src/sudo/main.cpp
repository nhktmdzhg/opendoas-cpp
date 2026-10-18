#include "doas/doas.hpp"
#include "sudo/sudo.hpp"
#include "sys/error.hpp"
#include "sys/io.hpp"
#include "sys/process.hpp"
#include "sys/users.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sudo {

namespace {

// show_cmd() of the original: the command is echoed to stderr unless SUDO_HIDE
// is present in the environment (an empty value counts as present).
bool hidden() {
    return sys::process::get_env("SUDO_HIDE").has_value();
}

void show_cmd(std::string_view prefix, const std::vector<std::string>& args) {
    if (hidden()) {
        return;
    }
    std::string line(prefix);
    const char* sep = "";
    for (const std::string& arg : args) {
        line += sep;
        line += arg;
        sep = " ";
    }
    line += "\n";
    sys::err_out(line);
}

void show_argv(std::string_view prefix, int argc, const char* const argv[]) {
    if (hidden()) {
        return;
    }
    std::string line(prefix);
    const char* sep = "";
    for (int i = 0; i < argc; i++) {
        line += sep;
        line += argv[i];
        sep = " ";
    }
    line += "\n";
    sys::err_out(line);
}

} // namespace

int main(int argc, char* const argv[]) {
    // Kept for parity with the original (which reports a missing doas binary and
    // exits with 255); the resolved path is only used for the trace output.
    const std::optional<std::string> doas_prog = sys::process::which("doas");
    if (!doas_prog) {
        sys::err_out("doas couldn't be found on your system.\n");
        return 255;
    }

    // Display the inputed sudo command
    show_argv("sudo <<< ", argc, argv);

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

    const TranslateResult translated = translate(argc, argv);
    if (translated.exit_now) {
        return 0;
    }
    for (const std::string& arg : translated.args) {
        args.push_back(arg);
    }

    // Display the translated doas command
    show_cmd("sudo >>> ", args);

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
