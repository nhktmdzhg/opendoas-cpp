// Scripted implementation of the privileged operations, linked by the doasedit
// tests in place of src/doasedit/elevated.cpp.
#include "fake_elevated.hpp"

#include "sys/fs.hpp"

#include <cerrno>

namespace doasedit::elevated {

namespace {

std::vector<Step> g_steps;
size_t g_next = 0;
std::vector<std::string> g_calls;
bool g_args_matched = true;
std::string g_written;

const Step* next(const std::string& op, const std::string& key) {
    g_calls.push_back(op + ":" + key);
    if (g_next >= g_steps.size()) {
        g_args_matched = false;
        return nullptr;
    }
    const Step& step = g_steps[g_next++];
    if (step.op != op || step.key != key) {
        g_args_matched = false;
    }
    return &step;
}

} // namespace

void script(const std::vector<Step>& steps) {
    g_steps = steps;
    g_next = 0;
    g_calls.clear();
    g_args_matched = true;
    g_written.clear();
}

const std::vector<std::string>& calls() {
    return g_calls;
}

bool script_consumed() {
    return g_args_matched && g_next == g_steps.size();
}

std::string written_content() {
    return g_written;
}

int authorize_read(std::string_view path) {
    const Step* step = next("authorize_read", std::string(path));
    return step != nullptr ? step->status : 1;
}

int authorize_write(std::string_view tmp_file, std::string_view target, int attempts) {
    (void)attempts;
    const Step* step = next("authorize_write", std::string(tmp_file) + ">" + std::string(target));
    return step != nullptr ? step->status : 1;
}

sys::Result<struct stat> stat_path(std::string_view path) {
    const Step* step = next("stat_path", std::string(path));
    if (step == nullptr || step->status != 0) {
        return std::unexpected(sys::fail_errno(ENOENT));
    }
    return step->st;
}

Text read_path(std::string_view path) {
    const Step* step = next("read_path", std::string(path));
    Text result;
    if (step == nullptr) {
        return result;
    }
    result.status = step->status;
    if (step->status == 0) {
        result.out = step->content;
    }
    return result;
}

int write_path(std::string_view path, std::string_view tmp_file, const sys::fs::Fd& source) {
    (void)tmp_file;
    const Step* step = next("write_path", std::string(path));
    if (auto content = sys::fs::read_all(source.get()); content) {
        g_written = *content;
    }
    return step != nullptr ? step->status : 1;
}

} // namespace doasedit::elevated
