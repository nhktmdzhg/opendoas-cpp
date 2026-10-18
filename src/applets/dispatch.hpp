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

// Which applet to run, decided by the basename of argv[0] (busybox style).
Applet detect(std::string_view argv0);

} // namespace applets

#endif // DOAS_APP_LETS_DISPATCH_HPP
