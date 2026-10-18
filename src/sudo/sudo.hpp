#ifndef DOAS_SUDO_SUDO_HPP
#define DOAS_SUDO_SUDO_HPP

#include <string>
#include <string_view>
#include <vector>

namespace sudo {

enum class Attr {
    kUnknown,
    kPass,
    kIgnore,
    kExit,
};

struct Equiv {
    std::string_view sudo_flag;
    const char* doas_flag;
    Attr attr;
    int nargs;
};

// The sudo -> doas flag translation table.
extern const Equiv kEquiv[];

struct TranslateResult {
    bool exit_now = false;
    std::vector<std::string> args;
};

// Pure translation of a sudo command line into doas arguments: same matching
// order as the table, same "--" passthrough and same per-flag argument
// consumption. Never exits and never writes output.
TranslateResult translate(int argc, const char* const argv[]);

// Applet entry point. Returns the process exit status (255 when doas is not
// found in PATH).
int main(int argc, char* const argv[]);

} // namespace sudo

#endif // DOAS_SUDO_SUDO_HPP
