#include "applets/dispatch.hpp"
#include "doas/doas.hpp"
#include "doasedit/doasedit.hpp"
#include "sudo/sudo.hpp"
#include "sys/error.hpp"
#include "sys/io.hpp"
#include "sys/process.hpp"

#include <cstdlib>
#include <string_view>
#include <system_error>

namespace applets {

Applet detect(std::string_view argv0) {
    const size_t slash = argv0.rfind('/');
    const std::string_view base =
        (slash == std::string_view::npos) ? argv0 : argv0.substr(slash + 1);
    if (base == "doas") {
        return Applet::kDoas;
    }
    if (base == "doasedit") {
        return Applet::kDoasedit;
    }
    if (base == "sudo") {
        return Applet::kSudo;
    }
    return Applet::kUnknown;
}

} // namespace applets

namespace {

// The doas applet needs the effective root privileges granted by the setuid
// installation; every other applet drops them immediately (keeping the saved set
// user id zero so that a forked doas child can raise it again, which mirrors the
// setuid exec the original binaries performed).
void drop_effective_privileges() {
    const uid_t real = sys::process::uid();
    if (sys::process::euid() == real) {
        // Not installed setuid: there is nothing to drop.
        return;
    }
    if (sys::Status dropped = sys::process::set_res_uid(real, real, 0); !dropped) {
        sys::die(1, dropped.error(), "setresuid");
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view argv0 = (argc > 0 && argv[0] != nullptr) ? argv[0] : "";

    switch (applets::detect(argv0)) {
    case applets::Applet::kDoas:
        // Applets are dispatched on argv[0], which the caller controls; the doas
        // applet only hands out the privileges a setuid /usr/bin/doas already
        // would, and every other applet drops them right away.
        doas::replace_main(argc, argv);
    case applets::Applet::kDoasedit:
        sys::set_progname("doasedit");
        drop_effective_privileges();
        return doasedit::main(argc, argv);
    case applets::Applet::kSudo:
        sys::set_progname("sudo");
        drop_effective_privileges();
        return sudo::main(argc, argv);
    case applets::Applet::kUnknown:
        break;
    }

    sys::err_out("doas-common: unknown applet; invoke as one of: doas, doasedit, sudo\n");
    return 1;
}
