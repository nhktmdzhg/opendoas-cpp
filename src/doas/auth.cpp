#include "doas/auth.hpp"

#include <algorithm>
#include <format>

namespace doas {

std::string password_prompt(std::string_view user, std::string_view host) {
    // snprintf(..., "\rdoas (%.32s@%.32s) password: ", myname, host): the %.32s
    // precision truncates by bytes.
    constexpr size_t max_length = 32;
    const std::string_view truncated_user = user.substr(0, std::min(user.size(), max_length));
    const std::string_view truncated_host = host.substr(0, std::min(host.size(), max_length));
    return std::format("\rdoas ({}@{}) password: ", truncated_user, truncated_host);
}

} // namespace doas
