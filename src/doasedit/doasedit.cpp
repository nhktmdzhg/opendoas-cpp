/*
 * Copyright (c) 2025 Nguyen Hoang Ky
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// Port of the doasedit Rust implementation (main.rs, file_handler.rs, editor.rs,
// utils.rs, error.rs). Every observable message, branch and quirk is kept; the
// only structural change is that the "doas ..." subprocesses are replaced by the
// in-process doas applet (see bridge.hpp) and that failures travel as
// sys::Error instead of pre-formatted strings.
#include "doasedit/doasedit.hpp"

#include "doas/config.hpp"
#include "doasedit/cli.hpp"
#include "doasedit/elevated.hpp"
#include "doasedit/path.hpp"
#include "sys/error.hpp"
#include "sys/fs.hpp"
#include "sys/io.hpp"
#include "sys/process.hpp"
#include "sys/strings.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace doasedit {

namespace {

class DoaseditCategory : public std::error_category {
  public:
    [[nodiscard]] const char* name() const noexcept override {
        return "doasedit";
    }

    [[nodiscard]] std::string message(int value) const override {
        switch (static_cast<Err>(value)) {
        case Err::kRootUser:
            return "using this program as root is not permitted";
        case Err::kNoDoas:
            return "unable to run 'doas'";
        case Err::kEditorInvalid:
            return "invalid editor command";
        case Err::kEditorFailed:
            return "editor exited with non-zero status";
        case Err::kCannotEditDir:
            return "cannot edit directories";
        case Err::kNotRegular:
            return "not a regular file";
        case Err::kOwnFile:
            return "editing your own files is not permitted";
        case Err::kOwnDir:
            return "creating files in your own directory is not permitted";
        case Err::kWritableDir:
            return "creating files in a user-writable directory is not permitted";
        case Err::kNoDirectory:
            return "no such directory";
        case Err::kReadableWritable:
            return "editing user-readable and -writable files is not permitted";
        case Err::kDoasCat:
            return "you are not permitted to call 'doas cat'";
        case Err::kThreeAttempts:
            return "3 incorrect password attempts";
        case Err::kInterrupted:
            return "interrupted";
        case Err::kUserAbort:
            return "aborted by user";
        case Err::kInvalidPath:
            return "Invalid file path";
        case Err::kTempDirectory:
            return "Error creating temporary directory";
        }
        return std::format("doasedit error {}", value);
    }
};

// Builds the error value. subject carries the path or the inner message that the
// original Rust code interpolated into the text.
sys::Error error_of(Err kind, std::string_view subject = {}) {
    return sys::with_subject(sys::Error{make_error_code(kind), {}}, subject);
}

constexpr std::string_view kPrompt = "(E)dit again, (O)verwrite anyway, (A)bort: [E/o/a]? ";

// ---------------------------------------------------------------------------
// Small text helpers (Rust semantics)
// ---------------------------------------------------------------------------

bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string trim_ascii(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && is_ascii_space(text[begin])) {
        begin++;
    }
    while (end > begin && is_ascii_space(text[end - 1])) {
        end--;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string join_path(std::string_view dir, std::string_view name) {
    return std::string(dir) + "/" + std::string(name);
}

// ---------------------------------------------------------------------------
// Status checks (utils.rs)
// ---------------------------------------------------------------------------

// True when the invoking user can open the file for reading. Replaces the Rust
// `permissions().readonly()` test, which was not a readability test at all: a root
// owned mode 0640 file (e.g. /etc/shadow) counted as "readable" and the plain copy
// then failed with EACCES.
bool user_can_read(std::string_view path) {
    const std::string name(path);
    const int fd = ::open(name.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        return false;
    }
    (void)::close(fd);
    return true;
}

// Builds FileInfo from a stat result. Readability is passed in because the caller
// decides how to probe it (user space open, or an elevated stat of a file the user
// cannot even see).
FileInfo info_from_stat(const struct stat& st, bool readable) {
    FileInfo info;
    info.exists = true;
    info.is_directory = S_ISDIR(st.st_mode);
    if (info.is_directory) {
        return info;
    }
    const unsigned int mode = static_cast<unsigned int>(st.st_mode & 07777);
    info.is_owned_by_user = st.st_uid == sys::process::uid();
    info.readable = readable;
    info.writable = (mode & 0222U) != 0 && info.is_owned_by_user;
    return info;
}

// ---------------------------------------------------------------------------
// editor.rs
// ---------------------------------------------------------------------------

std::string get_editor_command() {
    for (const char* name : {"DOAS_EDITOR", "VISUAL", "EDITOR"}) {
        const std::optional<std::string> value = sys::process::get_env(name);
        if (value && !value->empty() && sys::utf8_valid(*value)) {
            return *value;
        }
    }
    return std::string("vi");
}

// Reads one line from stdin, including its newline like Rust's read_line().
std::optional<std::string> read_stdin_line() {
    std::string line;
    for (;;) {
        char c = 0;
        const ssize_t got = ::read(STDIN_FILENO, &c, 1);
        if (got == 0) {
            return line;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
        line.push_back(c);
        if (c == '\n') {
            return line;
        }
    }
}

std::optional<std::string> read_user_input(std::string_view prompt) {
    sys::err_out(prompt);
    sys::flush_all();
    return read_stdin_line();
}

// Spawns the editor with inherited stdio, reporting spawn failures the way
// Rust's Command::status() does (through a pipe so the exec error survives).
sys::Status run_editor(const std::string& editor, const std::string& file) {
    auto pipe = sys::fs::make_pipe();
    if (!pipe) {
        return std::unexpected(error_of(Err::kEditorInvalid, prose(pipe.error())));
    }
    (void)sys::fs::set_close_on_exec(pipe->second.get());

    sys::flush_all();

    const auto child = sys::process::fork_child();
    if (!child) {
        return std::unexpected(error_of(Err::kEditorInvalid, prose(child.error())));
    }

    if (*child == 0) {
        pipe->first.reset();

        std::string prog(editor);
        std::string arg(file);
        char* argv[] = {prog.data(), arg.data(), nullptr};
        std::vector<char*> envp;
        for (char** entry = environ; entry != nullptr && *entry != nullptr; entry++) {
            envp.push_back(*entry);
        }
        envp.push_back(nullptr);

        const std::error_code error = sys::process::execv_search(prog, {argv, 2}, envp);
        const int code = error.value();
        (void)::write(pipe->second.get(), &code, sizeof(code));
        ::_exit(1);
    }

    pipe->second.reset();
    // The child reports the exec error as a raw int through the pipe.
    std::array<char, sizeof(int)> raw{};
    const auto got = sys::fs::read_fd(pipe->first.get(), {raw.data(), raw.size()});
    pipe->first.reset();
    int reported = 0;
    std::memcpy(&reported, raw.data(), sizeof(reported));

    const auto waited = sys::process::wait_for(*child);
    if (got && *got == raw.size()) {
        return std::unexpected(error_of(Err::kEditorInvalid, prose(sys::fail_errno(reported))));
    }
    if (!waited) {
        return std::unexpected(error_of(Err::kEditorFailed));
    }
    if (waited->exited() && waited->exit_code() == 0) {
        return {};
    }
    return std::unexpected(error_of(Err::kEditorFailed));
}

// The temp file the editor works on is the only thing of one edit that touches the
// disk: mode 0600, inside the caller's private temp directory.
sys::Status create_temp_file(std::string_view tmp_file) {
    if (sys::Status written = sys::fs::write_file(tmp_file, ""); !written) {
        return written;
    }
    if (sys::Status mode = sys::fs::chmod_path(tmp_file, 0600); !mode) {
        return mode;
    }
    return {};
}

// fill_comparison_copy(): the pre-edit content goes into the memfd, so that the
// comparison after the editor never reads a second copy from the disk. mlock() is
// best effort (a locked page cannot be swapped out).
sys::Status fill_comparison_copy(std::string_view tmp_file, const sys::fs::Fd& copy) {
    const auto content = sys::fs::read_file(tmp_file);
    if (!content) {
        return std::unexpected(content.error());
    }
    if (sys::Status written = sys::fs::write_all(copy.get(), *content); !written) {
        return written;
    }
    (void)sys::fs::mlock_span(*content);
    return sys::fs::rewind_fd(copy.get());
}

// copy_original_content()
sys::Status
copy_original_content(const std::string& original, const std::string& tmp_file, bool use_doas) {
    if (use_doas) {
        const elevated::Text result = elevated::read_path(original);
        if (result.status != 0) {
            return std::unexpected(error_of(Err::kDoasCat));
        }
        return sys::fs::write_file(tmp_file, result.out);
    }
    const auto copied = sys::fs::copy_file(original, tmp_file);
    if (!copied) {
        return std::unexpected(copied.error());
    }
    return {};
}

bool is_doas_config_file(std::string_view file_path) {
    return file_path.starts_with("/etc/doas") &&
           (file_path == "/etc/doas.conf" || file_path.starts_with("/etc/doas.d/"));
}

// validate_doas_config()
sys::Status validate_doas_config(const std::string& tmp_file, const std::string& editor) {
    for (;;) {
        const sys::Result<std::string> diagnostics = doas::validate_config(tmp_file);
        if (!diagnostics) {
            return std::unexpected(sys::with_subject(diagnostics.error(), tmp_file));
        }
        if (diagnostics->empty()) {
            break;
        }

        // `doas -C` printed these lines on stderr; keep that order.
        sys::err_out(*diagnostics);
        sys::err_out(std::format(
            "doasedit: Replacing '{}' would introduce the above error and break doas.\n",
            tmp_file));

        const std::optional<std::string> input = read_user_input(kPrompt);
        if (!input) {
            return std::unexpected(error_of(Err::kInterrupted));
        }

        const std::string answer = lower_ascii(trim_ascii(*input));
        if (answer == "o") {
            break;
        }
        if (answer == "a") {
            return std::unexpected(error_of(Err::kUserAbort));
        }
        const sys::Status edited = run_editor(editor, tmp_file);
        if (!edited) {
            return edited;
        }
    }
    return {};
}

// files_match(): the edited temp file against the pre-edit copy kept in the memfd.
sys::Result<bool> files_match(const std::string& path, const sys::fs::Fd& copy) {
    const auto edited = sys::fs::read_file(path);
    if (!edited) {
        return std::unexpected(edited.error());
    }
    // The memfd was written to, so its offset sits at the end.
    if (sys::Status rewound = sys::fs::rewind_fd(copy.get()); !rewound) {
        return std::unexpected(rewound.error());
    }
    const auto original = sys::fs::read_all(copy.get());
    if (!original) {
        return std::unexpected(original.error());
    }
    return *edited == *original;
}

// write_file_back(): the password, if any, was already asked before the editor
// opened; this only re-checks the rule for the "dd" command line.
sys::Status
write_file_back(const std::string& tmp_file, const std::string& original, bool writable) {
    if (writable) {
        const auto copied = sys::fs::copy_file(tmp_file, original);
        if (!copied) {
            return std::unexpected(copied.error());
        }
        return {};
    }

    const auto opened = sys::fs::open_file(tmp_file, O_RDONLY);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    if (elevated::write_path(original, tmp_file, *opened) != 0) {
        return std::unexpected(error_of(Err::kThreeAttempts));
    }
    return {};
}

} // namespace

const std::error_category& category() {
    static const DoaseditCategory instance;
    return instance;
}

std::error_code make_error_code(Err value) noexcept {
    return std::error_code(static_cast<int>(value), category());
}

std::string prose(const sys::Error& error) {
    if (error.code.category() == category()) {
        switch (static_cast<Err>(error.code.value())) {
        case Err::kRootUser:
        case Err::kNoDoas:
        case Err::kEditorFailed:
        case Err::kDoasCat:
        case Err::kThreeAttempts:
        case Err::kInterrupted:
        case Err::kUserAbort:
            return error.code.message();
        case Err::kEditorInvalid:
            return std::format("invalid editor command: '{}'", error.subject);
        case Err::kCannotEditDir:
        case Err::kNotRegular:
        case Err::kOwnFile:
        case Err::kOwnDir:
        case Err::kWritableDir:
        case Err::kNoDirectory:
        case Err::kReadableWritable:
            return std::format("{}: {}", error.subject, error.code.message());
        case Err::kInvalidPath:
            return std::format("invalid editor command: '{}'", error.code.message());
        case Err::kTempDirectory:
            return std::format("Error creating temporary directory: {}", error.subject);
        }
    }
    // Rust's io::Error Display: "<strerror> (os error <N>)".
    return std::format("{} (os error {})", error.code.message(), error.code.value());
}

sys::Result<FileInfo> check_file_status(std::string_view path) {
    const uid_t uid = sys::process::uid();

    // A plain stat answers the common case; only a permission failure means root
    // has to look, so a file that simply does not exist never costs a fork (and
    // never asks for a password before the policy check below).
    const sys::Result<struct stat> st = sys::fs::stat_follow(path);
    if (st) {
        return info_from_stat(*st, user_can_read(path));
    }

    const int stat_errno = st.error().errno_value();
    const bool needs_root = stat_errno == EACCES || stat_errno == EPERM;
    if (needs_root) {
        // A directory the user may not search, or a file only root can see: one
        // elevated stat answers existence, type and metadata (the applet used to
        // spend three forks on `doas test -e`, `doas test -f` and `doas stat`).
        const sys::Result<struct stat> elevated_stat = elevated::stat_path(path);
        if (elevated_stat) {
            return info_from_stat(*elevated_stat, user_can_read(path));
        }
    }

    // File doesn't exist, check directory.
    const std::string dir = path::parent_of(path);
    const sys::Result<struct stat> dir_stat = sys::fs::stat_follow(dir);
    if (dir_stat) {
        const unsigned int mode = static_cast<unsigned int>(dir_stat->st_mode & 07777);
        if (dir_stat->st_uid == uid) {
            return std::unexpected(error_of(Err::kOwnDir, path));
        }
        if ((mode & 0222U) != 0 && dir_stat->st_uid == uid) {
            return std::unexpected(error_of(Err::kWritableDir, path));
        }
    } else {
        const int dir_errno = dir_stat.error().errno_value();
        const bool dir_needs_root = dir_errno == EACCES || dir_errno == EPERM;
        if (!dir_needs_root || !elevated::stat_path(dir)) {
            return std::unexpected(error_of(Err::kNoDirectory, dir));
        }
    }

    return FileInfo{};
}

sys::Status
process_file(std::string_view file_path, std::string_view editor, std::string_view tmp_dir) {
    // Check if path is a directory (ends with /).
    if (path::is_directory_path(file_path)) {
        return std::unexpected(error_of(Err::kCannotEditDir, file_path));
    }

    if (!path::file_name_of(file_path)) {
        return std::unexpected(error_of(Err::kInvalidPath));
    }

    const auto status = check_file_status(file_path);
    if (!status) {
        return std::unexpected(status.error());
    }
    FileInfo info = *status;

    // The temp path is needed before the first authorization: the rule for the "dd"
    // command line carries it, and the write after the editor has to use the same
    // string for that rule to match again.
    const std::optional<std::string> filename = path::file_name_of(file_path);
    if (!filename) {
        return std::unexpected(error_of(Err::kInvalidPath));
    }
    const std::string tmp_file = join_path(tmp_dir, *filename);

    if (info.exists) {
        if (info.is_owned_by_user) {
            return std::unexpected(error_of(Err::kOwnFile, file_path));
        }
        if (info.is_directory) {
            return std::unexpected(error_of(Err::kNotRegular, file_path));
        }
        if (info.readable && info.writable) {
            return std::unexpected(error_of(Err::kReadableWritable, file_path));
        }
    } else {
        // A file that does not exist yet is created through doas by
        // write_file_back(), so the write always needs authorization.
        info.writable = false;
    }

    // The password is asked here, before the editor opens; every later operation of
    // this run only re-checks the rule.
    if (info.exists && !info.readable && elevated::authorize_read(file_path) != 0) {
        return std::unexpected(error_of(Err::kDoasCat));
    }
    if (!info.writable && elevated::authorize_write(tmp_file, file_path, 3) != 0) {
        return std::unexpected(error_of(Err::kThreeAttempts));
    }

    if (sys::Status created = create_temp_file(tmp_file); !created) {
        return created;
    }
    auto copy = sys::fs::memfd("doasedit-copy");
    if (!copy) {
        return std::unexpected(copy.error());
    }

    if (info.exists) {
        if (sys::Status copied =
                copy_original_content(std::string(file_path), tmp_file, !info.readable);
            !copied) {
            return copied;
        }
        if (sys::Status filled = fill_comparison_copy(tmp_file, *copy); !filled) {
            return filled;
        }
    }

    if (sys::Status edited = run_editor(std::string(editor), tmp_file); !edited) {
        return edited;
    }

    if (is_doas_config_file(file_path)) {
        if (sys::Status validated = validate_doas_config(tmp_file, std::string(editor));
            !validated) {
            return validated;
        }
    }

    const auto same = files_match(tmp_file, *copy);
    if (!same) {
        return std::unexpected(same.error());
    }
    if (!*same) {
        return write_file_back(tmp_file, std::string(file_path), info.writable);
    }

    sys::out(std::format("doasedit: {}: unchanged\n", file_path));
    return {};
}

int main(int argc, char* const argv[]) {
    const cli::ParseResult parsed = cli::parse(argc, argv);

    switch (parsed.status) {
    case cli::ParseResult::Status::kHelp:
        sys::out(cli::kHelpText);
        return 0;
    case cli::ParseResult::Status::kVersion:
        sys::out(cli::kVersionText);
        return 0;
    case cli::ParseResult::Status::kError:
        sys::err_out(parsed.error_text);
        return 2;
    case cli::ParseResult::Status::kOk:
        break;
    }

    // The dispatcher dropped the effective uid, so this is the real user id:
    // "running as root" means the invoking user really is root.
    if (sys::process::euid() == 0) {
        sys::err_out(std::format("doasedit: {}\n", prose(error_of(Err::kRootUser))));
        return 1;
    }

    const std::string editor = get_editor_command();

    auto tmp_dir = sys::fs::make_temp_dir();
    if (!tmp_dir) {
        sys::err_out(std::format("doasedit: {}\n",
                                 prose(error_of(Err::kTempDirectory, prose(tmp_dir.error())))));
        return 1;
    }

    for (const std::string& file : parsed.files) {
        const sys::Status status = process_file(file, editor, *tmp_dir);
        if (!status) {
            // Like Rust's process::exit(1): the temporary directory is not
            // removed on this path.
            sys::err_out(std::format("doasedit: {}\n", prose(status.error())));
            return 1;
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(*tmp_dir, ignored);
    return 0;
}

} // namespace doasedit
