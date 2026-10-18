// match_rule()/permit() ported from doas.c, exercised directly.
#include "doas/config.hpp"
#include "test_util.hpp"

#include <grp.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

doas::Rule rule(std::string ident,
                std::optional<std::string> target,
                std::optional<std::string> cmd,
                std::optional<std::vector<std::string>> cmdargs,
                int action = doas::kPermit) {
    doas::Rule r;
    r.action = action;
    r.ident = std::move(ident);
    r.target = std::move(target);
    r.cmd = std::move(cmd);
    r.cmdargs = std::move(cmdargs);
    return r;
}

std::vector<std::string> args(std::initializer_list<const char*> list) {
    std::vector<std::string> out;
    for (const char* item : list) {
        out.emplace_back(item);
    }
    return out;
}

} // namespace

int main() {
    const uid_t uid = getuid();
    const gid_t gid = getgid();
    std::vector<gid_t> groups = {gid, static_cast<gid_t>(gid + 1)};

    const doas::MatchContext ctx{uid, groups, 0};
    const std::string uid_text = std::to_string(static_cast<unsigned>(uid));
    const std::string gid_text = std::to_string(static_cast<unsigned>(gid));

    // Identity matching.
    CHECK(doas::match_rule(
        ctx, rule(uid_text, std::nullopt, std::nullopt, std::nullopt), "true", {}));
    CHECK(!doas::match_rule(
        ctx, rule("nouser", std::nullopt, std::nullopt, std::nullopt), "true", {}));
    CHECK(!doas::match_rule(
        ctx, rule("4294967294", std::nullopt, std::nullopt, std::nullopt), "true", {}));
    CHECK(doas::match_rule(
        ctx, rule(":" + gid_text, std::nullopt, std::nullopt, std::nullopt), "true", {}));
    CHECK(doas::match_rule(ctx,
                           rule(":" + std::to_string(static_cast<unsigned>(gid + 1)),
                                std::nullopt,
                                std::nullopt,
                                std::nullopt),
                           "true",
                           {}));
    CHECK(!doas::match_rule(ctx,
                            rule(":" + std::to_string(static_cast<unsigned>(gid + 2)),
                                 std::nullopt,
                                 std::nullopt,
                                 std::nullopt),
                            "true",
                            {}));
    CHECK(!doas::match_rule(
        ctx, rule(":4294967294", std::nullopt, std::nullopt, std::nullopt), "true", {}));
    // A name with a leading ':' that is not convertible to a gid never matches.
    CHECK(!doas::match_rule(ctx, rule(":", std::nullopt, std::nullopt, std::nullopt), "true", {}));

    // Target matching (the default target is 0).
    CHECK(doas::match_rule(
        ctx, rule(uid_text, std::nullopt, std::nullopt, std::nullopt), "true", {}));
    CHECK(doas::match_rule(
        ctx, rule(uid_text, std::string("0"), std::nullopt, std::nullopt), "true", {}));
    CHECK(!doas::match_rule(ctx, rule(uid_text, uid_text, std::nullopt, std::nullopt), "true", {}));
    {
        doas::MatchContext as_self{uid, groups, uid};
        CHECK(doas::match_rule(
            as_self, rule(uid_text, uid_text, std::nullopt, std::nullopt), "true", {}));
        CHECK(!doas::match_rule(
            as_self, rule(uid_text, "0", std::nullopt, std::nullopt), "true", {}));
    }

    // Command and argument matching.
    CHECK(doas::match_rule(
        ctx, rule(uid_text, std::nullopt, std::string("ls"), std::nullopt), "ls", {}));
    CHECK(!doas::match_rule(
        ctx, rule(uid_text, std::nullopt, std::string("ls"), std::nullopt), "cat", {}));
    CHECK(doas::match_rule(ctx,
                           rule(uid_text, std::nullopt, std::string("ls"), std::nullopt),
                           "ls",
                           args({"a", "b"})));
    CHECK(doas::match_rule(
        ctx, rule(uid_text, std::nullopt, std::string("ls"), args({"-l"})), "ls", args({"-l"})));
    CHECK(!doas::match_rule(
        ctx, rule(uid_text, std::nullopt, std::string("ls"), args({"-l"})), "ls", args({"-a"})));
    CHECK(!doas::match_rule(
        ctx, rule(uid_text, std::nullopt, std::string("ls"), args({"-l"})), "ls", {}));
    CHECK(!doas::match_rule(ctx,
                            rule(uid_text, std::nullopt, std::string("ls"), args({"-l"})),
                            "ls",
                            args({"-l", "extra"})));
    // An empty `args` clause means "the command must not take arguments".
    CHECK(doas::match_rule(
        ctx,
        rule(uid_text, std::nullopt, std::string("ls"), std::vector<std::string>{}),
        "ls",
        {}));
    CHECK(!doas::match_rule(
        ctx,
        rule(uid_text, std::nullopt, std::string("ls"), std::vector<std::string>{}),
        "ls",
        args({"x"})));
    // Without a cmd clause the arguments are irrelevant.
    CHECK(doas::match_rule(
        ctx, rule(uid_text, std::nullopt, std::nullopt, std::nullopt), "ls", args({"x"})));

    // permit(): the last matching rule wins.
    doas::g_rules.clear();
    doas::g_rules.push_back(rule(uid_text, std::nullopt, std::nullopt, std::nullopt));
    CHECK(doas::permit(ctx, "true", {}).allowed);

    doas::g_rules.clear();
    doas::g_rules.push_back(rule(uid_text, std::nullopt, std::nullopt, std::nullopt, doas::kDeny));
    CHECK(!doas::permit(ctx, "true", {}).allowed);

    doas::g_rules.clear();
    doas::g_rules.push_back(rule(uid_text, std::nullopt, std::nullopt, std::nullopt, doas::kDeny));
    doas::g_rules.push_back(rule(uid_text, std::nullopt, std::nullopt, std::nullopt));
    CHECK(doas::permit(ctx, "true", {}).allowed);

    doas::g_rules.clear();
    doas::g_rules.push_back(rule(uid_text, std::nullopt, std::nullopt, std::nullopt));
    doas::g_rules.push_back(rule(uid_text, std::nullopt, std::nullopt, std::nullopt, doas::kDeny));
    CHECK(!doas::permit(ctx, "true", {}).allowed);

    // A rule for somebody else does not match at all.
    doas::g_rules.clear();
    doas::g_rules.push_back(rule("nouser", std::nullopt, std::nullopt, std::nullopt));
    {
        const doas::PermitResult result = doas::permit(ctx, "true", {});
        CHECK(!result.allowed);
        CHECK(!result.rule.has_value());
    }

    // The returned rule is the one that decided.
    doas::g_rules.clear();
    doas::g_rules.push_back(rule(uid_text, std::nullopt, std::string("ls"), std::nullopt));
    doas::g_rules.push_back(rule(uid_text, std::nullopt, std::string("cat"), std::nullopt));
    {
        const doas::PermitResult result = doas::permit(ctx, "cat", {});
        CHECK(result.allowed);
        CHECK(result.rule.has_value());
        CHECK(result.rule.has_value() && result.rule->cmd.has_value() &&
              *result.rule->cmd == "cat");
    }

    doas::g_rules.clear();
    return test::finish("t_match");
}
