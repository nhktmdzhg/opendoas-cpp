// Link-time seam for the doasedit tests: instead of forking and running the doas
// applet, the bridge returns scripted results. Only the test binary links this
// file; the shipped binary always uses bridge.cpp.
#ifndef DOAS_TESTS_FAKE_BRIDGE_HPP
#define DOAS_TESTS_FAKE_BRIDGE_HPP

#include "doasedit/bridge.hpp"

#include <string>
#include <vector>

namespace doasedit::bridge {

struct Expected {
    std::vector<std::string> argv;
    Result result;
};

// Programs the next spawn_* calls. Unused steps are reported as a failure.
void script(const std::vector<Expected>& steps);

// The argv lists the code under test passed, in order.
const std::vector<std::vector<std::string>>& calls();

// True when every scripted step was consumed in order with matching arguments.
bool script_consumed();

} // namespace doasedit::bridge

#endif // DOAS_TESTS_FAKE_BRIDGE_HPP
