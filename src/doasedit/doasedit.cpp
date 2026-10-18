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

#include "doasedit/bridge.hpp"
#include "doasedit/cli.hpp"
#include "doasedit/path.hpp"
#include "sys/error.hpp"
#include "sys/fs.hpp"
#include "sys/io.hpp"
#include "sys/process.hpp"
#include "sys/strings.hpp"

#include <array>
#include <cerrno>
#include <charconv>
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
        case Err::kValidation:
            return "doas validation error";
        }
        return std::format("doasedit error {}", value);
    }
};

// Builds the error value. subject carries the path or the inner message that the
// original Rust code interpolated into the text.
sys::Error error_of(Err kind, std::string_view subject = {}) {
    return sys::with_subject(sys::Error{make_error_code(kind), {}}, subject);
}

constexpr std::string_view kCopyPrefix = "copy-of-";
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

std::vector<std::string_view> split_whitespace(std::string_view text) {
    std::vector<std::string_view> parts;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_ascii_space(text[i])) {
            i++;
        }
        const size_t start = i;
        while (i < text.size() && !is_ascii_space(text[i])) {
            i++;
        }
        if (i > start) {
            parts.push_back(text.substr(start, i - start));
        }
    }
    return parts;
}

std::optional<unsigned int> parse_u32(std::string_view text) {
    unsigned int value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<unsigned int> parse_u32_base8(std::string_view text) {
    unsigned int value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 8);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::string join_path(std::string_view dir, std::string_view name) {
    return std::string(dir) + "/" + std::string(name);
}

// ---------------------------------------------------------------------------
// Status checks (utils.rs)
// ---------------------------------------------------------------------------

std::optional<FileInfo> status_of(std::string_view path) {
    const auto st = sys::fs::stat_follow(path);
    if (!st) {
        return std::nullopt;
    }
    FileInfo info;
    info.exists = true;
    info.is_directory = S_ISDIR(st->st_mode);
    if (info.is_directory) {
        return info;
    }
    const unsigned int mode = static_cast<unsigned int>(st->st_mode & 07777);
    const uid_t uid = sys::process::uid();
    info.is_owned_by_user = st->st_uid == uid;
    info.readable = (mode & 0222U) != 0; // permissions().readonly() == false
    info.writable = (mode & 0222U) != 0 && st->st_uid == uid;
    return info;
}

// get_file_metadata_with_doas(): parses "doas stat -c '%u %a' <path>".
sys::Result<std::pair<uid_t, unsigned int>> metadata_with_doas(const std::string& path) {
    const bridge::Result result =
        bridge::spawn_capture({std::string("stat"), std::string("-c"), std::string("%u %a"), path});
    if (result.status != 0) {
        return std::unexpected(error_of(Err::kDoasCat));
    }

    const std::vector<std::string_view> parts = split_whitespace(result.out);
    if (parts.size() < 2) {
        return std::unexpected(error_of(Err::kValidation, "Invalid stat output"));
    }
    const std::optional<unsigned int> uid = parse_u32(parts[0]);
    if (!uid) {
        return std::unexpected(error_of(Err::kValidation, "Invalid UID format"));
    }
    const unsigned int mode = parse_u32_base8(parts[1]).value_or(0644U);
    return std::pair<uid_t, unsigned int>{static_cast<uid_t>(*uid), mode};
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

// create_secure_temp_copy()
sys::Result<std::pair<std::string, std::string>>
create_secure_temp_copy(std::string_view file_path, std::string_view temp_dir) {
    const std::optional<std::string> filename = path::file_name_of(file_path);
    if (!filename) {
        return std::unexpected(error_of(Err::kInvalidPath));
    }
    const std::string copy_name = std::string(kCopyPrefix) + *filename;

    const std::string tmp_file = join_path(temp_dir, *filename);
    const std::string tmp_copy = join_path(temp_dir, copy_name);

    if (sys::Status written = sys::fs::write_file(tmp_file, ""); !written) {
        return std::unexpected(written.error());
    }
    if (sys::Status written = sys::fs::write_file(tmp_copy, ""); !written) {
        return std::unexpected(written.error());
    }

    // Set secure permissions (0600).
    if (sys::Status mode = sys::fs::chmod_path(tmp_file, 0600); !mode) {
        return std::unexpected(mode.error());
    }
    if (sys::Status mode = sys::fs::chmod_path(tmp_copy, 0600); !mode) {
        return std::unexpected(mode.error());
    }
    return std::pair<std::string, std::string>{tmp_file, tmp_copy};
}

// copy_original_content()
sys::Status
copy_original_content(const std::string& original, const std::string& tmp_file, bool use_doas) {
    if (use_doas) {
        const bridge::Result result = bridge::spawn_capture({"cat", original});
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

// create_comparison_copy()
sys::Status create_comparison_copy(const std::string& tmp_file, const std::string& tmp_copy) {
    const auto copied = sys::fs::copy_file(tmp_file, tmp_copy);
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
        const bridge::Result result = bridge::spawn_discard({"-C", tmp_file});
        if (result.status == 0) {
            break;
        }

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

// files_match()
sys::Result<bool> files_match(const std::string& first, const std::string& second) {
    const auto content1 = sys::fs::read_file(first);
    if (!content1) {
        return std::unexpected(content1.error());
    }
    const auto content2 = sys::fs::read_file(second);
    if (!content2) {
        return std::unexpected(content2.error());
    }
    return *content1 == *content2;
}

// write_file_back()
sys::Status
write_file_back(const std::string& tmp_file, const std::string& original, bool writable) {
    if (writable) {
        const auto copied = sys::fs::copy_file(tmp_file, original);
        if (!copied) {
            return std::unexpected(copied.error());
        }
        return {};
    }

    // Try with doas (with retry for password).
    for (int attempt = 0; attempt < 3; attempt++) {
        const bridge::Result result =
            bridge::spawn_discard({"dd", "status=none", "if=" + tmp_file, "of=" + original});
        if (result.status == 0) {
            return {};
        }
        if (attempt == 2) {
            return std::unexpected(error_of(Err::kThreeAttempts));
        }
    }
    return std::unexpected(error_of(Err::kThreeAttempts));
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
        case Err::kValidation:
            return std::format("doas validation error: {}", error.subject);
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

    // First try without doas.
    if (const std::optional<FileInfo> info = status_of(path)) {
        return *info;
    }

    // Try with doas.
    const std::string target(path);
    const bridge::Result exists = bridge::spawn_discard({"test", "-e", target});
    if (exists.status == 0) {
        const bridge::Result is_file = bridge::spawn_discard({"test", "-f", target});
        if (is_file.status != 0) {
            FileInfo info;
            info.exists = true;
            info.is_directory = true;
            return info;
        }

        auto metadata = metadata_with_doas(target);
        if (!metadata) {
            return std::unexpected(metadata.error());
        }

        FileInfo info;
        info.exists = true;
        info.readable = true;                            // Assuming readable if doas can read it
        info.writable = (metadata->second & 0200U) != 0; // User write bit
        info.is_owned_by_user = metadata->first == uid;
        return info;
    }

    // File doesn't exist, check directory.
    const std::string dir = path::parent_of(path);
    const auto dir_stat = sys::fs::stat_follow(dir);
    if (dir_stat) {
        const unsigned int mode = static_cast<unsigned int>(dir_stat->st_mode & 07777);
        if (dir_stat->st_uid == uid) {
            return std::unexpected(error_of(Err::kOwnDir, path));
        }
        if ((mode & 0222U) != 0 && dir_stat->st_uid == uid) {
            return std::unexpected(error_of(Err::kWritableDir, path));
        }
    } else {
        const bridge::Result dir_exists = bridge::spawn_discard({"test", "-d", dir});
        if (dir_exists.status != 0) {
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
    const FileInfo info = *status;

    const auto paths = create_secure_temp_copy(file_path, tmp_dir);
    if (!paths) {
        return std::unexpected(paths.error());
    }
    const std::string& tmp_file = paths->first;
    const std::string& tmp_copy = paths->second;

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

        if (sys::Status copied =
                copy_original_content(std::string(file_path), tmp_file, !info.readable);
            !copied) {
            return copied;
        }

        if (sys::Status copied = create_comparison_copy(tmp_file, tmp_copy); !copied) {
            return copied;
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

    const auto same = files_match(tmp_file, tmp_copy);
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
