// The doasedit command line, byte-compatible with the clap generated output of
// doasedit 1.1.0.
#include "doasedit/cli.hpp"
#include "test_util.hpp"

#include <string>
#include <vector>

namespace {

doasedit::cli::ParseResult parse(std::initializer_list<const char*> argv) {
    std::vector<std::string> owned;
    for (const char* arg : argv) {
        owned.emplace_back(arg);
    }
    std::vector<const char*> raw;
    raw.reserve(owned.size());
    for (const std::string& arg : owned) {
        raw.push_back(arg.c_str());
    }
    return doasedit::cli::parse(static_cast<int>(raw.size()), raw.data());
}

std::string join(const std::vector<std::string>& values) {
    std::string out;
    const char* sep = "";
    for (const std::string& value : values) {
        out += sep;
        out += value;
        sep = "|";
    }
    return out;
}

} // namespace

int main() {
    using Status = doasedit::cli::ParseResult::Status;

    // Help text, pinned byte for byte; the version line comes from meta::applet_version.
    CHECK_EQ(std::string(doasedit::cli::kHelpText),
             std::string("doasedit (opendoas-cpp) - edit files as root with an unprivileged "
                         "editor\n"
                         "\n"
                         "Usage: doasedit [-h|--help] [-v|--version] <files>...\n"
                         "\n"
                         "Arguments:\n"
                         "  <files>...  Files to edit\n"
                         "\n"
                         "Options:\n"
                         "  -h, --help     Print help\n"
                         "  -v, --version  Print version\n"));

    {
        const auto result = parse({"doasedit", "--help"});
        CHECK(result.status == Status::kHelp);
    }
    {
        const auto result = parse({"doasedit", "-v"});
        CHECK(result.status == Status::kVersion);
    }
    // -V is no longer the version flag: it is an unknown argument now.
    {
        const auto result = parse({"doasedit", "-V"});
        CHECK(result.status == Status::kError);
    }
    // The flag that appears first wins.
    {
        const auto result = parse({"doasedit", "-v", "-h"});
        CHECK(result.status == Status::kVersion);
    }
    {
        const auto result = parse({"doasedit", "-h", "-v"});
        CHECK(result.status == Status::kHelp);
    }
    // Flags are recognised after positional arguments too.
    {
        const auto result = parse({"doasedit", "/etc/hosts", "--version"});
        CHECK(result.status == Status::kVersion);
    }

    // No arguments.
    {
        const auto result = parse({"doasedit"});
        CHECK(result.status == Status::kError);
        CHECK_EQ(result.error_text,
                 std::string("error: the following required arguments were not provided:\n"
                             "  <files>...\n"
                             "\n"
                             "Usage: doasedit [-h|--help] [-v|--version] <files>...\n"
                             "\n"
                             "For more information, try '--help'.\n"));
    }

    // Unknown flag.
    {
        const auto result = parse({"doasedit", "--bogus"});
        CHECK(result.status == Status::kError);
        CHECK_EQ(result.error_text,
                 std::string("error: unexpected argument '--bogus' found\n"
                             "\n"
                             "  tip: to pass '--bogus' as a value, use '-- --bogus'\n"
                             "\n"
                             "Usage: doasedit [-h|--help] [-v|--version] <files>...\n"
                             "\n"
                             "For more information, try '--help'.\n"));
    }

    // Positional arguments, in order.
    {
        const auto result = parse({"doasedit", "/etc/hosts", "/etc/fstab", "/etc/securetty"});
        CHECK(result.status == Status::kOk);
        CHECK_EQ(join(result.files), std::string("/etc/hosts|/etc/fstab|/etc/securetty"));
    }
    // "--" ends option parsing; a lone "-" is a file.
    {
        const auto result = parse({"doasedit", "--", "--help", "-"});
        CHECK(result.status == Status::kOk);
        CHECK_EQ(join(result.files), std::string("--help|-"));
    }
    {
        const auto result = parse({"doasedit", "-"});
        CHECK(result.status == Status::kOk);
        CHECK_EQ(join(result.files), std::string("-"));
    }

    return test::finish("t_doasedit_cli");
}
