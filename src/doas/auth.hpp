#ifndef DOAS_DOAS_AUTH_HPP
#define DOAS_DOAS_AUTH_HPP

#include <string>
#include <string_view>

namespace doas {

// PAM service name used by pam_start(3), as in the original pam.c.
constexpr const char* kPamServiceName = "doas";

// reproduces the "\rdoas (%.32s@%.32s) password: " prompt of pam.c/shadow.c,
// including the %.32s byte truncation.
std::string password_prompt(std::string_view user, std::string_view host);

#if defined(DOAS_HAVE_PAM)
// Port of pamauth() from pam.c.
void pamauth(std::string_view target_user,
             std::string_view self_user,
             bool interactive,
             bool nopass,
             bool persist);
#elif defined(DOAS_HAVE_SHADOW)
// Port of shadowauth() from shadow.c.
void shadow_auth(std::string_view user, bool persist);
#endif

} // namespace doas

#endif // DOAS_DOAS_AUTH_HPP
