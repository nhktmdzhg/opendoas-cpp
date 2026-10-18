// The doasedit file classifier and its error branches, driven through the
// scripted doas bridge (tests/fake_bridge.cpp). Error messages are checked through
// doasedit::prose(), i.e. exactly what the user sees.
#include "doasedit/doasedit.hpp"
#include "fake_bridge.hpp"
#include "sys/fs.hpp"
#include "test_util.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

namespace bridge = doasedit::bridge;

bridge::Result result(int status, std::string out = {}) {
    bridge::Result value;
    value.status = status;
    value.out = std::move(out);
    return value;
}

std::string make_temp_dir() {
    char tmpl[] = "/tmp/doasedit-policy-XXXXXX";
    const char* made = mkdtemp(tmpl);
    if (made == nullptr) {
        std::perror("mkdtemp");
        std::exit(2);
    }
    return std::string(made);
}

void write_file(const std::string& path, std::string_view content, mode_t mode) {
    CHECK(sys::fs::write_file(path, content).has_value());
    CHECK(sys::fs::chmod_path(path, mode).has_value());
}

} // namespace

int main() {
    namespace fs = std::filesystem;

    const std::string dir = make_temp_dir();
    const std::string plain = dir + "/plain";
    const std::string readonly = dir + "/readonly";
    const std::string subdir = dir + "/sub";
    const std::string blocked = dir + "/blocked";
    const std::string inner = blocked + "/inner";
    const std::string missing = dir + "/missing";
    const std::string no_parent = dir + "/nodir/file";

    write_file(plain, "hello\n", 0644);
    write_file(readonly, "hello\n", 0444);
    CHECK(sys::fs::mkdir_path(subdir, 0755).has_value());
    CHECK(sys::fs::mkdir_path(blocked, 0755).has_value());
    write_file(inner, "secret\n", 0444);
    CHECK(sys::fs::chmod_path(blocked, 0000).has_value());

    // Stat-able files and directories: no doas call at all.
    {
        bridge::script({});
        const auto status = doasedit::check_file_status(plain);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->exists);
            CHECK(status->readable);
            CHECK(status->writable);
            CHECK(status->is_owned_by_user);
            CHECK(!status->is_directory);
        }
        CHECK_EQ(bridge::calls().size(), static_cast<size_t>(0));
        CHECK(bridge::script_consumed());
    }

    // A write protected file counts as "not readable" in the original logic
    // (permissions().readonly()), so the doas copy path would be used.
    {
        bridge::script({});
        const auto status = doasedit::check_file_status(readonly);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->exists);
            CHECK(!status->readable);
            CHECK(!status->writable);
            CHECK(status->is_owned_by_user);
        }
    }

    // Directories are reported with exists = true and is_directory = true.
    {
        bridge::script({});
        const auto status = doasedit::check_file_status(subdir);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->exists);
            CHECK(status->is_directory);
            CHECK(!status->readable);
            CHECK(!status->writable);
            CHECK(!status->is_owned_by_user);
        }
    }

    // A new file in a directory owned by the invoking user is refused; the
    // message names the file, exactly like the original did.
    {
        bridge::script({});
        const auto status = doasedit::check_file_status(missing);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string(missing) +
                         ": creating files in your own directory is not permitted");
        }
    }

    // A new file whose parent directory does not exist: the two doas test calls
    // decide.
    {
        bridge::script(
            {{{"test", "-e", no_parent}, result(1)}, {{"test", "-d", dir + "/nodir"}, result(1)}});
        const auto status = doasedit::check_file_status(no_parent);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string(dir + "/nodir") + ": no such directory");
        }
        CHECK(bridge::script_consumed());
    }

    // Unstat-able file: the doas fallback classifies it through stat(1).
    {
        bridge::script({{{"test", "-e", inner}, result(0)},
                        {{"test", "-f", inner}, result(0)},
                        {{"stat", "-c", "%u %a", inner}, result(0, "0 440\n")}});
        const auto status = doasedit::check_file_status(inner);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->exists);
            CHECK(status->readable);
            CHECK(!status->writable);
            CHECK(!status->is_owned_by_user);
        }
        CHECK(bridge::script_consumed());
    }

    // Owner write bit set -> writable.
    {
        bridge::script({{{"test", "-e", inner}, result(0)},
                        {{"test", "-f", inner}, result(0)},
                        {{"stat", "-c", "%u %a", inner}, result(0, "0 200")}});
        const auto status = doasedit::check_file_status(inner);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->writable);
        }
        CHECK(bridge::script_consumed());
    }

    // Not a regular file -> reported as a directory.
    {
        bridge::script({{{"test", "-e", inner}, result(0)}, {{"test", "-f", inner}, result(1)}});
        const auto status = doasedit::check_file_status(inner);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->exists);
            CHECK(status->is_directory);
        }
        CHECK(bridge::script_consumed());
    }

    // A failing doas stat is reported with the doas cat message.
    {
        bridge::script({{{"test", "-e", inner}, result(0)},
                        {{"test", "-f", inner}, result(0)},
                        {{"stat", "-c", "%u %a", inner}, result(1)}});
        const auto status = doasedit::check_file_status(inner);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string("you are not permitted to call 'doas cat'"));
        }
        CHECK(bridge::script_consumed());
    }

    // Malformed stat output.
    {
        bridge::script({{{"test", "-e", inner}, result(0)},
                        {{"test", "-f", inner}, result(0)},
                        {{"stat", "-c", "%u %a", inner}, result(0, "garbage")}});
        const auto status = doasedit::check_file_status(inner);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string("doas validation error: Invalid stat output"));
        }
    }
    {
        bridge::script({{{"test", "-e", inner}, result(0)},
                        {{"test", "-f", inner}, result(0)},
                        {{"stat", "-c", "%u %a", inner}, result(0, "x 440")}});
        const auto status = doasedit::check_file_status(inner);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string("doas validation error: Invalid UID format"));
        }
    }
    // An unparsable mode falls back to 0644, whose owner write bit makes the file
    // "writable" for the doas branch.
    {
        bridge::script({{{"test", "-e", inner}, result(0)},
                        {{"test", "-f", inner}, result(0)},
                        {{"stat", "-c", "%u %a", inner}, result(0, "0 zzz")}});
        const auto status = doasedit::check_file_status(inner);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->writable);
        }
    }

    // process_file(): a trailing slash is refused before anything else.
    {
        bridge::script({});
        const auto status = doasedit::process_file(dir + "/", "/bin/true", dir);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string(dir + "/") + ": cannot edit directories");
        }
    }

    // A path without a final component has no file name.
    {
        bridge::script({});
        const auto status = doasedit::process_file(dir + "/..", "/bin/true", dir);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string("invalid editor command: 'Invalid file path'"));
        }
    }

    // Editing your own file is refused.
    {
        bridge::script({});
        const auto status = doasedit::process_file(plain, "/bin/true", dir);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string(plain) + ": editing your own files is not permitted");
        }
    }

    // A file that is not owned by the user but cannot be read: the direct copy
    // fails with the io error, like the original's fs::copy.
    {
        bridge::script({{{"test", "-e", inner}, result(0)},
                        {{"test", "-f", inner}, result(0)},
                        {{"stat", "-c", "%u %a", inner}, result(0, "0 444")}});
        const auto status = doasedit::process_file(inner, "/bin/true", dir);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string("Permission denied (os error 13)"));
        }
    }

    CHECK(sys::fs::chmod_path(blocked, 0755).has_value());
    std::error_code ignored;
    fs::remove_all(dir, ignored);

    return test::finish("t_doasedit_policy");
}
