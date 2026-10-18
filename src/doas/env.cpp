#include "doas/env.hpp"

#include "sys/process.hpp"

#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace doas {

namespace {

// name[1024] / keybuf[1024] in the original: longer names are ignored.
constexpr size_t kMaxName = 1023;

using EnvMap = std::map<std::string, std::string>;

// addnode(): inserts without overwriting an existing key.
void add_node(EnvMap& env, const std::string& key, const std::string& value) {
    env.emplace(key, value);
}

// fillenv(): parses "NAME", "NAME=value", "NAME=$OTHER", "-NAME" and
// "-NAME=value" entries, deleting any previous entry for the name first.
template <typename Container>
void fill_env(EnvMap& env, const Container& list, std::string_view formerpath) {
    for (const auto& raw : list) {
        const std::string_view entry(raw);

        const size_t eq = entry.find('=');
        const bool has_eq = eq != std::string_view::npos;
        const size_t len = has_eq ? eq : entry.size();
        if (len > kMaxName) {
            continue;
        }
        const std::string name(entry.substr(0, len));

        // delete previous copies
        std::string key = name;
        if (!key.empty() && key[0] == '-') {
            key = key.substr(1);
        }
        env.erase(key);
        if (!name.empty() && name[0] == '-') {
            continue;
        }

        // assign value or inherit from environ
        std::optional<std::string> val;
        if (has_eq) {
            const std::string_view value = entry.substr(eq + 1);
            if (!value.empty() && value[0] == '$') {
                const std::string_view ref = value.substr(1);
                if (ref == "PATH") {
                    val = std::string(formerpath);
                } else {
                    val = sys::process::get_env(ref);
                }
            } else {
                val = std::string(value);
            }
        } else {
            if (name == "PATH") {
                val = std::string(formerpath);
            } else {
                val = sys::process::get_env(name);
            }
        }
        // at last, we have something to insert
        if (val) {
            env.insert_or_assign(name, *val);
        }
    }
}

EnvMap create_env(const Rule& rule, const sys::PasswdEntry& self, const sys::PasswdEntry& target) {
    static const std::string_view copyset[] = {"DISPLAY", "TERM"};

    EnvMap env;

    // addnode() with strdup(): a name missing from the environment would have
    // crashed the original, which cannot happen here because doas sets PATH
    // before calling prepenv().
    add_node(env, "DOAS_USER", self.name);
    add_node(env, "HOME", target.dir);
    add_node(env, "LOGNAME", target.name);
    add_node(env, "PATH", sys::process::get_env("PATH").value_or(""));
    add_node(env, "SHELL", target.shell);
    add_node(env, "USER", target.name);

    fill_env(env, copyset, "");

    if ((rule.options & kKeepenv) != 0) {
        for (size_t i = 0; environ[i] != nullptr; i++) {
            const std::string_view e(environ[i]);
            const size_t eq = e.find('=');
            // ignore invalid or overlong names
            if (eq == std::string_view::npos || eq == 0) {
                continue;
            }
            if (eq > kMaxName) {
                continue;
            }
            // ignore any later duplicates
            env.emplace(std::string(e.substr(0, eq)), std::string(e.substr(eq + 1)));
        }
    }

    return env;
}

} // namespace

std::vector<std::string> prepenv(const Rule& rule,
                                 const sys::PasswdEntry& self,
                                 const sys::PasswdEntry& target,
                                 std::string_view formerpath) {
    EnvMap env = create_env(rule, self, target);
    if (rule.envlist) {
        fill_env(env, *rule.envlist, formerpath);
    }

    std::vector<std::string> out;
    out.reserve(env.size());
    for (const auto& entry : env) {
        out.push_back(entry.first + "=" + entry.second);
    }
    return out;
}

} // namespace doas
