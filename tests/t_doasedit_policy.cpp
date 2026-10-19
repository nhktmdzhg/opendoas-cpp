// The doasedit file classifier and its error branches, driven through the scripted
// privileged operations (tests/fake_elevated.cpp). Error messages are checked through
// doasedit::prose(), i.e. exactly what the user sees.
#include "doasedit/doasedit.hpp"
#include "fake_elevated.hpp"
#include "sys/fs.hpp"
#include "test_util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

namespace elevated = doasedit::elevated;

elevated::Step call(std::string op, std::string key, int status = 0) {
    elevated::Step step;
    step.op = std::move(op);
    step.key = std::move(key);
    step.status = status;
    return step;
}

elevated::Step stat_call(std::string path, uid_t owner, mode_t mode) {
    elevated::Step step = call("stat_path", std::move(path));
    step.st.st_uid = owner;
    step.st.st_mode = mode;
    return step;
}

elevated::Step read_call(std::string path, std::string content) {
    elevated::Step step = call("read_path", std::move(path));
    step.content = std::move(content);
    return step;
}

// "op:key" as recorded by the fake.
std::string call_name(std::string_view op, std::string_view key) {
    std::string out(op);
    out += ':';
    out += key;
    return out;
}

// "<tmp>>><target>", the key used for authorize_write.
std::string write_key(std::string_view tmp_file, std::string_view target) {
    std::string out(tmp_file);
    out += '>';
    out += target;
    return out;
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
    const std::string tmp_file = dir + "/inner";

    write_file(plain, "hello\n", 0644);
    write_file(readonly, "hello\n", 0444);
    CHECK(sys::fs::mkdir_path(subdir, 0755).has_value());
    CHECK(sys::fs::mkdir_path(blocked, 0755).has_value());
    write_file(inner, "secret\n", 0444);
    // A directory the user may not search: stat(2) then fails with EACCES, which is
    // what makes the elevated stat necessary.
    CHECK(sys::fs::chmod_path(blocked, 0000).has_value());

    // Stat-able, user owned file: no privileged call at all.
    {
        elevated::script({});
        const auto status = doasedit::check_file_status(plain);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->exists);
            CHECK(status->readable);
            CHECK(status->writable);
            CHECK(status->is_owned_by_user);
            CHECK(!status->is_directory);
        }
        CHECK_EQ(elevated::calls().size(), static_cast<size_t>(0));
        CHECK(elevated::script_consumed());
    }

    // A user owned write protected file is readable (the user can open it) but not
    // writable: readability is probed with open(2), not inferred from the mode.
    {
        elevated::script({});
        const auto status = doasedit::check_file_status(readonly);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->exists);
            CHECK(status->readable);
            CHECK(!status->writable);
            CHECK(status->is_owned_by_user);
        }
    }

    // Directories are reported with exists = true and is_directory = true.
    {
        elevated::script({});
        const auto status = doasedit::check_file_status(subdir);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->exists);
            CHECK(status->is_directory);
            CHECK(!status->readable);
            CHECK(!status->writable);
            CHECK(!status->is_owned_by_user);
        }
        CHECK_EQ(elevated::calls().size(), static_cast<size_t>(0));
    }

    // A missing file never costs a privileged call: the stat error is ENOENT, so the
    // policy check runs first and refuses it in the user's own directory.
    {
        elevated::script({});
        const auto status = doasedit::check_file_status(missing);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string(missing) +
                         ": creating files in your own directory is not permitted");
        }
        CHECK_EQ(elevated::calls().size(), static_cast<size_t>(0));
        CHECK(elevated::script_consumed());
    }

    // A missing file whose parent directory does not exist: no privileged call, the
    // directory stat already answers ENOENT.
    {
        elevated::script({});
        const auto status = doasedit::check_file_status(no_parent);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string(dir + "/nodir") + ": no such directory");
        }
        CHECK_EQ(elevated::calls().size(), static_cast<size_t>(0));
    }

    // Unstat-able file (EACCES): one elevated stat classifies it.
    {
        elevated::script({stat_call(inner, 0, 0640)});
        const auto status = doasedit::check_file_status(inner);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->exists);
            CHECK(!status->is_directory);
            CHECK(!status->readable);
            CHECK(!status->writable);
            CHECK(!status->is_owned_by_user);
        }
        CHECK_EQ(elevated::calls(), std::vector<std::string>{call_name("stat_path", inner)});
        CHECK(elevated::script_consumed());
    }

    // Owner write bit set on an elevated stat: still not writable, because the file
    // belongs to root.
    {
        elevated::script({stat_call(inner, 0, 0620)});
        const auto status = doasedit::check_file_status(inner);
        CHECK(status.has_value());
        if (status) {
            CHECK(!status->writable);
            CHECK(!status->is_owned_by_user);
        }
        CHECK(elevated::script_consumed());
    }

    // A directory the user cannot stat: the elevated stat reports is_directory.
    {
        elevated::script({stat_call(inner, 0, S_IFDIR | 0700)});
        const auto status = doasedit::check_file_status(inner);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->exists);
            CHECK(status->is_directory);
        }
        CHECK(elevated::script_consumed());
    }

    // Elevated stat fails too: the policy check falls back to the directory checks,
    // and the parent directory is owned by the user, so creating a file there is
    // refused.
    {
        elevated::script({call("stat_path", inner, EACCES)});
        const auto status = doasedit::check_file_status(inner);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string(inner) +
                         ": creating files in your own directory is not permitted");
        }
        const std::vector<std::string> want{call_name("stat_path", inner)};
        CHECK_EQ(elevated::calls(), want);
        CHECK(elevated::script_consumed());
    }

    // process_file(): a trailing slash is refused before anything else.
    {
        elevated::script({});
        const auto status = doasedit::process_file(dir + "/", "/bin/true", dir);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string(dir + "/") + ": cannot edit directories");
        }
        CHECK_EQ(elevated::calls().size(), static_cast<size_t>(0));
    }

    // A path without a final component has no file name.
    {
        elevated::script({});
        const auto status = doasedit::process_file(dir + "/..", "/bin/true", dir);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string("invalid editor command: 'Invalid file path'"));
        }
        CHECK_EQ(elevated::calls().size(), static_cast<size_t>(0));
    }

    // Editing your own file is refused, before any authorization.
    {
        elevated::script({});
        const auto status = doasedit::process_file(plain, "/bin/true", dir);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string(plain) + ": editing your own files is not permitted");
        }
        CHECK_EQ(elevated::calls().size(), static_cast<size_t>(0));
    }

    // A file the user cannot read and cannot write: the read and the write are
    // authorized before the editor opens, then the content is read back as root. The
    // editor changes nothing, so the file counts as unchanged.
    {
        elevated::script({stat_call(inner, 0, 0640),
                          call("authorize_read", inner),
                          call("authorize_write", write_key(tmp_file, inner)),
                          read_call(inner, "secret\n")});
        const auto status = doasedit::process_file(inner, "/bin/true", dir);
        CHECK(status.has_value());
        const std::vector<std::string> want{
            call_name("stat_path", inner),
            call_name("authorize_read", inner),
            call_name("authorize_write", write_key(tmp_file, inner)),
            call_name("read_path", inner)};
        CHECK_EQ(elevated::calls(), want);
        CHECK(elevated::script_consumed());
    }

    // The editor changes the temp file: the content goes back through write_path,
    // which is the only privileged write of the run (no second password prompt).
    {
        const std::string editor = dir + "/editor.sh";
        write_file(editor, "#!/bin/sh\nexec truncate -s 0 \"$1\"\n", 0755);
        elevated::script({stat_call(inner, 0, 0640),
                          call("authorize_read", inner),
                          call("authorize_write", write_key(tmp_file, inner)),
                          read_call(inner, "secret\n"),
                          call("write_path", inner)});
        const auto status = doasedit::process_file(inner, editor, dir);
        CHECK(status.has_value());
        const std::vector<std::string> want{
            call_name("stat_path", inner),
            call_name("authorize_read", inner),
            call_name("authorize_write", write_key(tmp_file, inner)),
            call_name("read_path", inner),
            call_name("write_path", inner)};
        CHECK_EQ(elevated::calls(), want);
        CHECK_EQ(elevated::written_content(), std::string());
        CHECK(elevated::script_consumed());
    }

    // The read is refused: the doas cat message, and nothing else happens.
    {
        elevated::script({stat_call(inner, 0, 0640), call("authorize_read", inner, EACCES)});
        const auto status = doasedit::process_file(inner, "/bin/true", dir);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()),
                     std::string("you are not permitted to call 'doas cat'"));
        }
        const std::vector<std::string> want{call_name("stat_path", inner),
                                            call_name("authorize_read", inner)};
        CHECK_EQ(elevated::calls(), want);
        CHECK(elevated::script_consumed());
    }

    // The write is refused: 3 incorrect password attempts, reported before the editor
    // opens.
    {
        elevated::script({stat_call(inner, 0, 0640),
                          call("authorize_read", inner),
                          call("authorize_write", write_key(tmp_file, inner), EACCES)});
        const auto status = doasedit::process_file(inner, "/bin/true", dir);
        CHECK(!status.has_value());
        if (!status) {
            CHECK_EQ(doasedit::prose(status.error()), std::string("3 incorrect password attempts"));
        }
        const std::vector<std::string> want{
            call_name("stat_path", inner),
            call_name("authorize_read", inner),
            call_name("authorize_write", write_key(tmp_file, inner))};
        CHECK_EQ(elevated::calls(), want);
        CHECK(elevated::script_consumed());
    }

    // A user owned file that others may write to: readable and writable, so no
    // privileged call at all.
    {
        const std::string world_file = dir + "/world";
        write_file(world_file, "x\n", 0666);
        elevated::script({});
        const auto status = doasedit::check_file_status(world_file);
        CHECK(status.has_value());
        if (status) {
            CHECK(status->readable);
            CHECK(status->writable);
        }
        CHECK_EQ(elevated::calls().size(), static_cast<size_t>(0));
    }

    CHECK(sys::fs::chmod_path(blocked, 0755).has_value());
    std::error_code ignored;
    fs::remove_all(dir, ignored);

    return test::finish("t_doasedit_policy");
}
