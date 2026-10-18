#ifndef DOAS_DOAS_ENV_HPP
#define DOAS_DOAS_ENV_HPP

#include "doas/config.hpp"
#include "sys/users.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace doas {

// Port of prepenv() from env.c. Returns the "KEY=VALUE" entries sorted by key
// (the order the original RB tree iterated in).
std::vector<std::string> prepenv(const Rule& rule,
                                 const sys::PasswdEntry& self,
                                 const sys::PasswdEntry& target,
                                 std::string_view formerpath);

} // namespace doas

#endif // DOAS_DOAS_ENV_HPP
