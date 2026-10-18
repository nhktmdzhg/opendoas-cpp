// prepenv() ported from env.c: the environment list, its order and the setenv()
// rewriting rules.
#include "doas/config.hpp"
#include "doas/env.hpp"
#include "sys/users.hpp"
#include "test_util.hpp"

#include <cstdlib>
#include <string>
#include <vector>

extern char** environ;

namespace {

sys::PasswdEntry self_entry() {
    sys::PasswdEntry entry;
    entry.name = "selfuser";
    entry.dir = "/home/selfuser";
    entry.shell = "/bin/selfshell";
    entry.uid = 1000;
    entry.gid = 1000;
    return entry;
}

sys::PasswdEntry target_entry() {
    sys::PasswdEntry entry;
    entry.name = "targetuser";
    entry.dir = "/root";
    entry.shell = "/bin/targetshell";
    entry.uid = 0;
    entry.gid = 0;
    return entry;
}

std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        out += value;
        out += '\n';
    }
    return out;
}

std::vector<std::string> setenv_list(std::initializer_list<const char*> list) {
    std::vector<std::string> out;
    for (const char* item : list) {
        out.emplace_back(item);
    }
    return out;
}

void reset_environment() {
    // Start from a known, small environment.
    clearenv();
    setenv("PATH", "/bin:/usr/bin", 1);
}

} // namespace

int main() {
    const sys::PasswdEntry self = self_entry();
    const sys::PasswdEntry target = target_entry();
    const std::string formerpath = "/former/path";

    // Default rule: the six base variables only (DISPLAY/TERM are unset).
    {
        reset_environment();
        const doas::Rule rule;
        const std::vector<std::string> env = doas::prepenv(rule, self, target, formerpath);
        CHECK_EQ(join(env),
                 std::string("DOAS_USER=selfuser\n"
                             "HOME=/root\n"
                             "LOGNAME=targetuser\n"
                             "PATH=/bin:/usr/bin\n"
                             "SHELL=/bin/targetshell\n"
                             "USER=targetuser\n"));
    }

    // DISPLAY and TERM are inherited when present.
    {
        reset_environment();
        setenv("DISPLAY", ":0", 1);
        setenv("TERM", "xterm", 1);
        const doas::Rule rule;
        const std::vector<std::string> env = doas::prepenv(rule, self, target, formerpath);
        CHECK_EQ(join(env),
                 std::string("DISPLAY=:0\n"
                             "DOAS_USER=selfuser\n"
                             "HOME=/root\n"
                             "LOGNAME=targetuser\n"
                             "PATH=/bin:/usr/bin\n"
                             "SHELL=/bin/targetshell\n"
                             "TERM=xterm\n"
                             "USER=targetuser\n"));
    }

    // keepenv: the whole environment is kept, base variables win on duplicates.
    {
        reset_environment();
        setenv("FOO", "bar", 1);
        setenv("EMPTY", "", 1);
        setenv("PATH", "/custom/bin", 1);
        doas::Rule rule;
        rule.options = doas::kKeepenv;
        const std::vector<std::string> env = doas::prepenv(rule, self, target, formerpath);
        // PATH comes from the process environment and is inserted before the
        // keepenv merge, so the duplicate from environ does not overwrite it.
        CHECK_EQ(join(env),
                 std::string("DOAS_USER=selfuser\n"
                             "EMPTY=\n"
                             "FOO=bar\n"
                             "HOME=/root\n"
                             "LOGNAME=targetuser\n"
                             "PATH=/custom/bin\n"
                             "SHELL=/bin/targetshell\n"
                             "USER=targetuser\n"));
    }

    // keepenv ignores names longer than 1023 bytes.
    {
        reset_environment();
        const std::string long_name(1024, 'a');
        setenv(long_name.c_str(), "x", 1);
        doas::Rule rule;
        rule.options = doas::kKeepenv;
        const std::vector<std::string> env = doas::prepenv(rule, self, target, formerpath);
        for (const std::string& entry : env) {
            CHECK(!entry.starts_with(long_name));
        }
    }

    // setenv: keep an inherited variable.
    {
        reset_environment();
        setenv("DISPLAY", ":1", 1);
        doas::Rule rule;
        rule.options = doas::kKeepenv;
        rule.envlist = setenv_list({"DISPLAY"});
        const std::vector<std::string> env = doas::prepenv(rule, self, target, formerpath);
        CHECK_EQ(join(env),
                 std::string("DISPLAY=:1\n"
                             "DOAS_USER=selfuser\n"
                             "HOME=/root\n"
                             "LOGNAME=targetuser\n"
                             "PATH=/bin:/usr/bin\n"
                             "SHELL=/bin/targetshell\n"
                             "USER=targetuser\n"));
    }

    // setenv: assign a literal value.
    {
        reset_environment();
        doas::Rule rule;
        rule.envlist = setenv_list({"TERM=xterm-256color"});
        const std::vector<std::string> env = doas::prepenv(rule, self, target, formerpath);
        CHECK_EQ(join(env),
                 std::string("DOAS_USER=selfuser\n"
                             "HOME=/root\n"
                             "LOGNAME=targetuser\n"
                             "PATH=/bin:/usr/bin\n"
                             "SHELL=/bin/targetshell\n"
                             "TERM=xterm-256color\n"
                             "USER=targetuser\n"));
    }

    // setenv: remove a variable.
    {
        reset_environment();
        doas::Rule rule;
        rule.envlist = setenv_list({"-HOME", "-PATH"});
        const std::vector<std::string> env = doas::prepenv(rule, self, target, formerpath);
        CHECK_EQ(join(env),
                 std::string("DOAS_USER=selfuser\n"
                             "LOGNAME=targetuser\n"
                             "SHELL=/bin/targetshell\n"
                             "USER=targetuser\n"));
    }

    // setenv: $PATH resolves to the former path, other $VARS to the environment.
    {
        reset_environment();
        setenv("SOURCE", "value", 1);
        doas::Rule rule;
        rule.envlist = setenv_list({"PATH=$PATH", "COPY=$SOURCE", "MISSING=$NOPE"});
        const std::vector<std::string> env = doas::prepenv(rule, self, target, formerpath);
        CHECK_EQ(join(env),
                 std::string("COPY=value\n"
                             "DOAS_USER=selfuser\n"
                             "HOME=/root\n"
                             "LOGNAME=targetuser\n"
                             "PATH=/former/path\n"
                             "SHELL=/bin/targetshell\n"
                             "USER=targetuser\n"));
    }

    // setenv: a bare name is inherited, an unknown name is dropped, and a name
    // longer than 1023 bytes is ignored entirely.
    {
        reset_environment();
        setenv("KNOWN", "yes", 1);
        doas::Rule rule;
        rule.envlist = setenv_list({"KNOWN", "UNKNOWN", "PATH"});
        std::string long_entry(1024, 'b');
        long_entry += "=x";
        rule.envlist->push_back(long_entry);
        const std::vector<std::string> env = doas::prepenv(rule, self, target, formerpath);
        CHECK_EQ(join(env),
                 std::string("DOAS_USER=selfuser\n"
                             "HOME=/root\n"
                             "KNOWN=yes\n"
                             "LOGNAME=targetuser\n"
                             "PATH=/former/path\n"
                             "SHELL=/bin/targetshell\n"
                             "USER=targetuser\n"));
    }

    // setenv entries that start with '-' remove, they are not kept.
    {
        reset_environment();
        setenv("DROP", "x", 1);
        doas::Rule rule;
        rule.options = doas::kKeepenv;
        rule.envlist = setenv_list({"-DROP"});
        const std::vector<std::string> env = doas::prepenv(rule, self, target, formerpath);
        for (const std::string& entry : env) {
            CHECK(!entry.starts_with("DROP="));
        }
    }

    clearenv();
    return test::finish("t_env");
}
