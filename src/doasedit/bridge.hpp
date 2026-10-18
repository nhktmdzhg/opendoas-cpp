#ifndef DOAS_DOASEDIT_BRIDGE_HPP
#define DOAS_DOASEDIT_BRIDGE_HPP

#include <initializer_list>
#include <string>
#include <string_view>

namespace doasedit::bridge {

struct Result {
    int status = 1;
    std::string out;
};

// Replacements for Rust's Command::new("doas")...output(): fork a child that runs
// the in-process doas applet with the given arguments instead of exec'ing the
// doas binary. The child raises its effective uid back to 0 from the saved set
// uid, exactly like the setuid exec the original used.
//
// spawn_capture: stdin <- /dev/null, stdout -> pipe (returned in Result::out),
//                stderr -> /dev/null.
// spawn_discard: stdin <- /dev/null, stdout and stderr -> /dev/null.
Result spawn_capture(std::initializer_list<std::string_view> argv);
Result spawn_discard(std::initializer_list<std::string_view> argv);

} // namespace doasedit::bridge

#endif // DOAS_DOASEDIT_BRIDGE_HPP
