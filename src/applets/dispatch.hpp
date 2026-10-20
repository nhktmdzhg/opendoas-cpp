#ifndef DOAS_APP_LETS_DISPATCH_HPP
#define DOAS_APP_LETS_DISPATCH_HPP

#include <string_view>

namespace applets {

enum class Applet {
    kDoas,
    kDoasedit,
    kSudo,
    kUnknown,
};

// Last component of argv[0]: the caller controls it, which is what selects the
// applet (busybox style).
[[nodiscard]] std::string_view basename(std::string_view argv0);

// Which applet to run, decided by the basename of argv[0].
Applet detect(std::string_view argv0);

} // namespace applets

#endif // DOAS_APP_LETS_DISPATCH_HPP
