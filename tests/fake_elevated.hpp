// Link-time seam for the doasedit tests: instead of forking and running doas's
// authorization for real, the privileged operations return scripted results. Only
// test binaries link this file; the shipped binary always uses elevated.cpp.
#ifndef DOAS_TESTS_FAKE_ELEVATED_HPP
#define DOAS_TESTS_FAKE_ELEVATED_HPP

#include "doasedit/elevated.hpp"

#include <string>
#include <sys/stat.h>
#include <vector>

namespace doasedit::elevated {

// One scripted call. `op` names the function ("authorize_read", "authorize_write",
// "stat_path", "read_path", "write_path") and `key` is what the test expects as its
// first argument (the path, or "<tmp>>><target>" for authorize_write).
struct Step {
    std::string op;
    std::string key;
    int status = 0;      // return value of authorize_*/write_path, or errno of read_path
    std::string content; // payload of read_path
    struct stat st{};    // payload of stat_path
};

// Programs the next calls; leftover scripted steps are a failure.
void script(const std::vector<Step>& steps);

// "op:key" for every call the code under test made, in order.
const std::vector<std::string>& calls();

// True when every scripted step was consumed in order with matching arguments.
bool script_consumed();

// What the last write_path() read from its source file descriptor.
std::string written_content();

} // namespace doasedit::elevated

#endif // DOAS_TESTS_FAKE_ELEVATED_HPP
