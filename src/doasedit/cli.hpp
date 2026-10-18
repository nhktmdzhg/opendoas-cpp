#ifndef DOAS_DOASEDIT_CLI_HPP
#define DOAS_DOASEDIT_CLI_HPP

#include <string>
#include <string_view>
#include <vector>

namespace doasedit::cli {

// Byte-for-byte clap output (captured from doasedit 1.1.0).
inline constexpr std::string_view kHelpText =
    "A Rust implementation of doasedit - edit files as root using an unprivileged editor\n"
    "\n"
    "Usage: doasedit <files>...\n"
    "\n"
    "Arguments:\n"
    "  <files>...  Files to edit\n"
    "\n"
    "Options:\n"
    "  -h, --help     Print help\n"
    "  -V, --version  Print version\n";

inline constexpr std::string_view kVersionText = "doasedit 1.1.0\n";

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

// clap-compatible parsing: the first of -h/--help/-V/--version found in argv
// wins, "--" ends option parsing and any other argument starting with '-' and
// longer than one byte is an error.
ParseResult parse(int argc, const char* const argv[]);

} // namespace doasedit::cli

#endif // DOAS_DOASEDIT_CLI_HPP
