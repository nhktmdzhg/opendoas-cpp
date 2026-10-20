#ifndef DOAS_VERSION_HPP
#define DOAS_VERSION_HPP

#include <format>
#include <string>
#include <string_view>

namespace meta {

// Identity of the multicall binary. Bump kVersion by hand; "dev" until a release.
inline constexpr std::string_view kProjectName = "opendoas-cpp";
inline constexpr std::string_view kVersion = "dev";

// "<applet> (opendoas-cpp) dev\n": the line every applet prints for -v/--version.
[[nodiscard]] inline std::string applet_version(std::string_view applet) {
    return std::format("{} ({}) {}\n", applet, kProjectName, kVersion);
}

} // namespace meta

#endif // DOAS_VERSION_HPP
