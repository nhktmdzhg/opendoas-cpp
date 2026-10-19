#ifndef DOAS_DOAS_CONFIG_HPP
#define DOAS_DOAS_CONFIG_HPP

#include "sys/error.hpp"

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace doas {

constexpr int kPermit = 1;
constexpr int kDeny = 2;

constexpr unsigned kNopass = 0x1;
constexpr unsigned kKeepenv = 0x2;
constexpr unsigned kPersist = 0x4;
constexpr unsigned kNolog = 0x8;

// One doas.conf rule. cmdargs/envlist distinguish "clause absent" (nullopt) from
// "clause present but empty" (engaged empty vector): an empty `args` list means
// the command must be called without arguments, while an absent one disables the
// argument check entirely.
struct Rule {
    int action = 0;
    unsigned options = 0;
    std::string ident;
    std::optional<std::string> target;
    std::optional<std::string> cmd;
    std::optional<std::vector<std::string>> cmdargs;
    std::optional<std::vector<std::string>> envlist;
};

// Rules in file order; the last matching rule decides.
extern std::vector<Rule> g_rules;
extern int g_parse_errors;

struct MatchContext {
    uid_t uid = 0;
    std::span<const gid_t> groups;
    uid_t target = 0;
};

struct PermitResult {
    bool allowed = false;
    std::optional<Rule> rule; // a copy of the deciding rule: no aliasing into g_rules
};

// Ports of match()/permit() from doas.c.
bool match_rule(const MatchContext& ctx,
                const Rule& rule,
                std::string_view cmd,
                std::span<const std::string> cmdargs);
PermitResult
permit(const MatchContext& ctx, std::string_view cmd, std::span<const std::string> cmdargs);

// Ports of parseuid()/parsegid(); nullopt means "not a known name and not a valid
// number in range".
std::optional<uid_t> parse_uid(std::string_view text);
std::optional<gid_t> parse_gid(std::string_view text);

// Port of parseconfig(); exits on error exactly like the original.
void parse_config(std::string_view filename, bool checkperms);

// Parser diagnostics sink: while a collector is active (only inside validate_config()) the text is
// appended to it, otherwise it goes to stderr, which is what the applet wants. Declared as a
// function so that no pointer to the storage ever crosses a translation unit boundary.
void report_config_error(std::string_view text);

// parse_config() without the fatal paths: parses `filename` with checkperms = false (the same thing
// `doas -C <file>` did) and returns the diagnostics the parser collected, empty when the file is
// valid. Only an unreadable file is an error.
[[nodiscard]] sys::Result<std::string> validate_config(std::string_view filename);

// The compiled-in path of doas.conf (DOAS_CONF), for callers that need to pass it explicitly. Keeps
// the macro inside doas_core.
[[nodiscard]] std::string_view config_path();

// Entry point implemented by parse.y: runs the grammar over fp.
int run_parser(std::FILE* fp);

} // namespace doas

#endif // DOAS_DOAS_CONFIG_HPP
