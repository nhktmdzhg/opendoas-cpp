#ifndef DOAS_DOASEDIT_CLI_HPP
#define DOAS_DOASEDIT_CLI_HPP

#include <string>
#include <string_view>
#include <vector>

namespace doasedit::cli {

// Help and usage text of the applet. The version line comes from meta::applet_version.
inline constexpr std::string_view kHelpText =
    "doasedit (opendoas-cpp) - edit files as root with an unprivileged editor\n"
    "\n"
    "Usage: doasedit [-h|--help] [-v|--version] <files>...\n"
    "\n"
    "Arguments:\n"
    "  <files>...  Files to edit\n"
    "\n"
    "Options:\n"
    "  -h, --help     Print help\n"
    "  -v, --version  Print version\n";

struct ParseResult {
    enum class Status {
        kOk,
        kHelp,
        kVersion,
        kError,
    };

    Status status = Status::kOk;
    std::vector<std::string> files;
    std::string error_text; // complete stderr text for Status::kError
};

// clap-compatible parsing: the first of -h/--help/-v/--version found in argv wins,
// "--" ends option parsing and any other argument starting with '-' and longer than
// one byte is an error.
ParseResult parse(int argc, const char* const argv[]);

} // namespace doasedit::cli

#endif // DOAS_DOASEDIT_CLI_HPP
