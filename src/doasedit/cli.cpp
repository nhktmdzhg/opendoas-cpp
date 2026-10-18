#include "doasedit/cli.hpp"

#include <format>
#include <string>
#include <string_view>

namespace doasedit::cli {

namespace {

// The tail shared by both clap errors.
std::string usage_footer() {
    return "Usage: doasedit <files>...\n\nFor more information, try '--help'.\n";
}

} // namespace

ParseResult parse(int argc, const char* const argv[]) {
    ParseResult result;
    bool end_of_options = false;

    for (int i = 1; i < argc; i++) {
        const std::string_view arg(argv[i]);
        if (!end_of_options) {
            if (arg == "--") {
                end_of_options = true;
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                result.status = ParseResult::Status::kHelp;
                return result;
            }
            if (arg == "-V" || arg == "--version") {
                result.status = ParseResult::Status::kVersion;
                return result;
            }
            if (arg.size() > 1 && arg[0] == '-') {
                result.status = ParseResult::Status::kError;
                result.error_text =
                    std::format("error: unexpected argument '{}' found\n\n  tip: to pass '{}' as a "
                                "value, use '-- {}'\n\n{}",
                                arg,
                                arg,
                                arg,
                                usage_footer());
                return result;
            }
        }
        result.files.emplace_back(arg);
    }

    if (result.files.empty()) {
        result.status = ParseResult::Status::kError;
        result.error_text =
            std::format("error: the following required arguments were not provided:\n  "
                        "<files>...\n\n{}",
                        usage_footer());
        return result;
    }

    result.status = ParseResult::Status::kOk;
    return result;
}

} // namespace doasedit::cli
