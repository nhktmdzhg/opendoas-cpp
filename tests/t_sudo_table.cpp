// The sudo -> doas translation table and translate().
#include "sudo/sudo.hpp"
#include "test_util.hpp"

#include <string>
#include <vector>

namespace {

std::string join(const std::vector<std::string>& args) {
    std::string out;
    const char* sep = "";
    for (const std::string& arg : args) {
        out += sep;
        out += arg;
        sep = " ";
    }
    return out;
}

std::vector<std::string> translate_line(std::initializer_list<const char*> argv) {
    std::vector<std::string> owned;
    for (const char* arg : argv) {
        owned.emplace_back(arg);
    }
    std::vector<const char*> raw;
    raw.reserve(owned.size());
    for (const std::string& arg : owned) {
        raw.push_back(arg.c_str());
    }
    return sudo::translate(static_cast<int>(raw.size()), raw.data()).args;
}

bool exits(std::initializer_list<const char*> argv) {
    std::vector<std::string> owned;
    for (const char* arg : argv) {
        owned.emplace_back(arg);
    }
    std::vector<const char*> raw;
    raw.reserve(owned.size());
    for (const std::string& arg : owned) {
        raw.push_back(arg.c_str());
    }
    return sudo::translate(static_cast<int>(raw.size()), raw.data()).exit_now;
}

} // namespace

int main() {
    // Every row of the documented table.
    CHECK_EQ(join(translate_line({"sudo", "-A", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--askpass", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-B", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--bell", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-b", "x"})), std::string("-n x"));
    CHECK_EQ(join(translate_line({"sudo", "--background", "x"})), std::string("-n x"));
    CHECK_EQ(join(translate_line({"sudo", "-C", "3", "x"})), std::string("3 x"));
    CHECK_EQ(join(translate_line({"sudo", "--close-from=3", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-D", "/tmp", "x"})), std::string("/tmp x"));
    CHECK_EQ(join(translate_line({"sudo", "--chdir=/tmp", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-E", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--preserve-env", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--preserve-env=PATH", "x"})), std::string("x"));
    CHECK(exits({"sudo", "-e"}));
    CHECK(exits({"sudo", "--edit"}));
    CHECK_EQ(join(translate_line({"sudo", "-g", "wheel", "x"})), std::string("wheel x"));
    CHECK_EQ(join(translate_line({"sudo", "--group=wheel", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-H", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--set-home", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--help", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-h", "host", "x"})), std::string("host x"));
    CHECK_EQ(join(translate_line({"sudo", "--host=host", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-i"})), std::string("-s"));
    CHECK_EQ(join(translate_line({"sudo", "-K", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--remove-timestamp", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-k", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--reset-timestamp", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-l", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--list", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-N", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--no-update", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-n", "x"})), std::string("-n x"));
    CHECK_EQ(join(translate_line({"sudo", "--non-interactive", "x"})), std::string("-n x"));
    CHECK_EQ(join(translate_line({"sudo", "-P", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--preserve-groups", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-p", "prompt", "x"})), std::string("prompt x"));
    CHECK_EQ(join(translate_line({"sudo", "--prompt=prompt", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-R", "dir", "x"})), std::string("dir x"));
    CHECK_EQ(join(translate_line({"sudo", "--chroot=dir", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-S", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--stdin", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-s"})), std::string("-s"));
    CHECK_EQ(join(translate_line({"sudo", "--shell"})), std::string("-s"));
    CHECK_EQ(join(translate_line({"sudo", "-U", "other", "x"})), std::string("other x"));
    CHECK_EQ(join(translate_line({"sudo", "--other-user=other", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-T", "5", "x"})), std::string("5 x"));
    CHECK_EQ(join(translate_line({"sudo", "--command-timeout=5", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-u", "root", "id"})), std::string("-u root id"));
    CHECK_EQ(join(translate_line({"sudo", "--user=root", "id"})), std::string("-u root id"));
    CHECK_EQ(join(translate_line({"sudo", "-V", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--version", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "-v", "x"})), std::string("x"));
    CHECK_EQ(join(translate_line({"sudo", "--validate", "x"})), std::string("x"));

    // No arguments at all.
    CHECK_EQ(join(translate_line({"sudo"})), std::string(""));

    // Passthrough once an unknown argument is seen, and after "--".
    CHECK_EQ(join(translate_line({"sudo", "whoami"})), std::string("whoami"));
    CHECK_EQ(join(translate_line({"sudo", "/bin/echo", "hi"})), std::string("/bin/echo hi"));
    CHECK_EQ(join(translate_line({"sudo", "--", "-n", "id"})), std::string("-n id"));

    // Quirks of the original loop, kept on purpose:
    // - "-u=root" matches "-u" and consumes the *next* argument,
    CHECK_EQ(join(translate_line({"sudo", "-u=root", "id"})), std::string("-u id"));
    // - IGNORE entries never consume their argument even when nargs is set.
    CHECK_EQ(join(translate_line({"sudo", "-p", "prompt", "id"})), std::string("prompt id"));

    // Combining translated and passed through flags.
    CHECK_EQ(join(translate_line({"sudo", "-n", "-u", "root", "--", "id", "-a"})),
             std::string("-n -u root id -a"));

    return test::finish("t_sudo_table");
}
