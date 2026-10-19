// The sudo command line is translated into doas arguments, which are then handed
// to the in-process doas applet instead of exec'ing a doas binary.
#include "sudo/sudo.hpp"

#include <cstring>
#include <vector>

namespace sudo {

const Equiv kEquiv[] = {
    {"-A", nullptr, Attr::kIgnore, 0},
    {"--askpass", nullptr, Attr::kIgnore, 0},
    {"-B", nullptr, Attr::kIgnore, 0},
    {"--bell", nullptr, Attr::kIgnore, 0},
    {"-b", "-n", Attr::kPass, 0},           // Run in background
    {"--background", "-n", Attr::kPass, 0}, // Run in background
    {"-C", nullptr, Attr::kIgnore, 1},
    {"--close-from=", nullptr, Attr::kIgnore, 0},
    {"-D", nullptr, Attr::kIgnore, 1},
    {"--chdir=", nullptr, Attr::kIgnore, 0},
    {"-E", nullptr, Attr::kIgnore, 0},
    {"--preserve-env", nullptr, Attr::kIgnore, 0},
    {"--preserve-env=", nullptr, Attr::kIgnore, 0},
    {"-e", nullptr, Attr::kExit, 0},
    {"--edit", nullptr, Attr::kExit, 0},
    {"-g", nullptr, Attr::kIgnore, 1},
    {"--group=", nullptr, Attr::kIgnore, 0},
    {"-H", nullptr, Attr::kIgnore, 0},
    {"--set-home", nullptr, Attr::kIgnore, 0},
    {"--help", nullptr, Attr::kIgnore, 0},
    {"-h", nullptr, Attr::kIgnore, 1},
    {"--host=", nullptr, Attr::kIgnore, 0},
    {"-i", "-s", Attr::kPass, 0}, // Execute user's shell
    {"-K", nullptr, Attr::kIgnore, 0},
    {"--remove-timestamp", nullptr, Attr::kIgnore, 0},
    {"-k", nullptr, Attr::kIgnore, 0},
    {"--reset-timestamp", nullptr, Attr::kIgnore, 0},
    {"-l", nullptr, Attr::kIgnore, 0},
    {"--list", nullptr, Attr::kIgnore, 0},
    {"-N", nullptr, Attr::kIgnore, 0},
    {"--no-update", nullptr, Attr::kIgnore, 0},
    {"-n", "-n", Attr::kPass, 0},                // Run in background
    {"--non-interactive", "-n", Attr::kPass, 0}, // Run in background
    {"-P", nullptr, Attr::kIgnore, 0},
    {"--preserve-groups", nullptr, Attr::kIgnore, 0},
    {"-p", nullptr, Attr::kIgnore, 1},
    {"--prompt=", nullptr, Attr::kIgnore, 0},
    {"-R", nullptr, Attr::kIgnore, 1},
    {"--chroot=", nullptr, Attr::kIgnore, 0},
    {"-S", nullptr, Attr::kIgnore, 0},
    {"--stdin", nullptr, Attr::kIgnore, 0},
    {"-s", "-s", Attr::kPass, 0},      // Execute user's shell
    {"--shell", "-s", Attr::kPass, 0}, // Execute user's shell
    {"-U", nullptr, Attr::kIgnore, 1},
    {"--other-user=", nullptr, Attr::kIgnore, 0},
    {"-T", nullptr, Attr::kIgnore, 1},
    {"--command-timeout=", nullptr, Attr::kIgnore, 0},
    {"-u", "-u", Attr::kPass, 1},      // Run as user
    {"--user=", "-u", Attr::kPass, 0}, // Run as user
    {"-V", nullptr, Attr::kIgnore, 0},
    {"--version", nullptr, Attr::kIgnore, 0},
    {"-v", nullptr, Attr::kIgnore, 0},
    {"--validate", nullptr, Attr::kIgnore, 0},
    {"", nullptr, Attr::kUnknown, 0},
};

namespace {

Equiv find_equiv(const char* arg) {
    for (const Equiv& entry : kEquiv) {
        if (entry.sudo_flag.empty()) {
            break;
        }
        if (std::strncmp(entry.sudo_flag.data(), arg, entry.sudo_flag.size()) == 0) {
            return entry;
        }
    }

    // arg isn't a sudo arg so just pass it to doas
    return Equiv{arg, nullptr, Attr::kUnknown, 0};
}

// Returns the value of a long form flag such as "--user=root", or nullptr.
const char* parse_long_flag(const Equiv& opt, const char* arg) {
    const size_t len = opt.sudo_flag.size();
    if (len == 0 || opt.sudo_flag[len - 1] != '=') {
        return nullptr;
    }
    return arg + len;
}

} // namespace

TranslateResult translate(int argc, const char* const argv[]) {
    TranslateResult result;
    bool passthru = false;

    for (int i = 1; i < argc; i++) {
        if (passthru) {
            result.args.emplace_back(argv[i]);
            continue;
        }

        if (std::strcmp("--", argv[i]) == 0) {
            passthru = true;
            continue;
        }

        const Equiv opt = find_equiv(argv[i]);
        if (opt.attr == Attr::kUnknown) {
            passthru = true;
            result.args.emplace_back(argv[i]);
            continue;
        }

        switch (opt.attr) {
        case Attr::kPass: {
            if (opt.doas_flag != nullptr) {
                result.args.emplace_back(opt.doas_flag);
            } else {
                result.args.emplace_back(opt.sudo_flag);
            }

            const char* long_value = parse_long_flag(opt, argv[i]);
            if (long_value != nullptr) {
                // It's a long flag, such as --user=root
                result.args.emplace_back(long_value);
            } else if (opt.nargs != 0) {
                // Flag has arguments that follow it
                i++;
                for (int j = 0; j < opt.nargs && i < argc; j++) {
                    i += j;
                    result.args.emplace_back(argv[i]);
                }
            }
            break;
        }
        case Attr::kIgnore:
            break;
        case Attr::kExit:
            result.exit_now = true;
            return result;
        case Attr::kUnknown:
            break;
        }
    }

    return result;
}

} // namespace sudo
