#ifndef DOAS_DOAS_AUTHORIZE_HPP
#define DOAS_DOAS_AUTHORIZE_HPP

#include "doas/config.hpp"

#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace doas {

// kAuthenticate asks the provider for a password (unless the rule is nopass or a
// still valid timestamp exists); kAlreadyAuthenticated is for the follow up
// operations of one doasedit run, where the rule is still matched but the password
// is not asked again.
enum class AuthMode {
    kAuthenticate,
    kAlreadyAuthenticated,
};

// Rule matching and authentication for one command line, without executing it:
// parse_config(config, true), permit(), the authentication provider (PAM or
// shadow), then the "ran command" syslog line unless the rule has NOLOG.
[[nodiscard]] Rule authorize_command(std::string_view cmd,
                                     std::span<const std::string> args,
                                     uid_t target = 0,
                                     AuthMode mode = AuthMode::kAuthenticate,
                                     bool interactive = true,
                                     std::string_view config = {});

} // namespace doas

#endif // DOAS_DOAS_AUTHORIZE_HPP
