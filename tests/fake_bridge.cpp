// Scripted implementation of the doas bridge, linked by the doasedit tests in
// place of src/doasedit/bridge.cpp.
#include "fake_bridge.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace doasedit::bridge {

namespace {

std::vector<Expected> g_steps;
size_t g_next = 0;
std::vector<std::vector<std::string>> g_calls;
bool g_args_matched = true;

Result dispatch(std::initializer_list<std::string_view> argv) {
    std::vector<std::string> call;
    call.reserve(argv.size());
    for (const std::string_view arg : argv) {
        call.emplace_back(arg);
    }
    g_calls.push_back(call);

    if (g_next >= g_steps.size()) {
        g_args_matched = false;
        return Result{};
    }
    const Expected& step = g_steps[g_next++];
    if (step.argv != call) {
        g_args_matched = false;
    }
    return step.result;
}

} // namespace

void script(const std::vector<Expected>& steps) {
    g_steps = steps;
    g_next = 0;
    g_calls.clear();
    g_args_matched = true;
}

const std::vector<std::vector<std::string>>& calls() {
    return g_calls;
}

bool script_consumed() {
    return g_args_matched && g_next == g_steps.size();
}

Result spawn_capture(std::initializer_list<std::string_view> argv) {
    return dispatch(argv);
}

Result spawn_discard(std::initializer_list<std::string_view> argv) {
    return dispatch(argv);
}

} // namespace doasedit::bridge
