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
// in-process doas applet (see bridge.hpp).
#include "doasedit/doasedit.hpp"

#include "doasedit/bridge.hpp"
#include "doasedit/cli.hpp"
#include "doasedit/path.hpp"
#include "sys/fs.hpp"
#include "sys/io.hpp"
#include "sys/process.hpp"
#include "sys/strings.hpp"

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace doasedit {

namespace {

// ---------------------------------------------------------------------------
// Messages (verbatim from error.rs and the call sites)
// ---------------------------------------------------------------------------

// Rust's io::Error Display for an OS error.
std::string rust_io_msg(const std::error_code& error) {
    return std::format("{} (os error {})", std::strerror(error.value()), error.value());
}

std::string invalid_editor(std::string_view msg) {
    return std::format("invalid editor command: '{}'", msg);
}

std::string cannot_edit_directory(std::string_view path) {
    return std::format("{}: cannot edit directories", path);
}

std::string not_regular_file(std::string_view path) {
    return std::format("{}: not a regular file", path);
}

std::string cannot_edit_own_file(std::string_view path) {
    return std::format("{}: editing your own files is not permitted", path);
}

std::string cannot_create_file_in_own_dir(std::string_view path) {
    return std::format("{}: creating files in your own directory is not permitted", path);
}

std::string cannot_create_file_in_writable_dir(std::string_view path) {
    return std::format("{}: creating files in a user-writable directory is not permitted", path);
}

std::string no_directory_exists(std::string_view path) {
    return std::format("{}: no such directory", path);
}

std::string cannot_edit_readable_writable_file(std::string_view path) {
    return std::format("{}: editing user-readable and -writable files is not permitted", path);
}

std::string doas_cat_permission_denied() {
    return "you are not permitted to call 'doas cat'";
}

std::string doas_validation_error(std::string_view msg) {
    return std::format("doas validation error: {}", msg);
}

std::string three_incorrect_password_attempts() {
    return "3 incorrect password attempts";
}

constexpr std::string_view kInterrupted = "interrupted";
constexpr std::string_view kEditorError = "editor exited with non-zero status";
constexpr std::string_view kUserAbort = "aborted by user";
constexpr std::string_view kInvalidFilePath = "Invalid file path";
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

std::optional<FileInfo> status_of(std::string_view path, std::string& error) {
    const sys::fs::StatResult st = sys::fs::stat_follow(path);
    if (st.error) {
        return std::nullopt;
    }
    FileInfo info;
    info.exists = true;
    info.is_directory = S_ISDIR(st.st.st_mode);
    if (info.is_directory) {
        error.clear();
        return info;
    }
    const unsigned int mode = static_cast<unsigned int>(st.st.st_mode & 07777);
    const uid_t uid = sys::process::uid();
    info.is_owned_by_user = st.st.st_uid == uid;
    info.readable = (mode & 0222U) != 0; // permissions().readonly() == false
    info.writable = (mode & 0222U) != 0 && st.st.st_uid == uid;
    error.clear();
    return info;
}

// get_file_metadata_with_doas(): parses "doas stat -c '%u %a' <path>".
std::optional<std::pair<uid_t, unsigned int>> metadata_with_doas(const std::string& path,
                                                                 std::string& error) {
    const bridge::Result result =
        bridge::spawn_capture({std::string("stat"), std::string("-c"), std::string("%u %a"), path});
    if (result.status != 0) {
        error = doas_cat_permission_denied();
        return std::nullopt;
    }

    const std::vector<std::string_view> parts = split_whitespace(result.out);
    if (parts.size() < 2) {
        error = doas_validation_error("Invalid stat output");
        return std::nullopt;
    }
    const std::optional<unsigned int> uid = parse_u32(parts[0]);
    if (!uid) {
        error = doas_validation_error("Invalid UID format");
        return std::nullopt;
    }
    const unsigned int mode = parse_u32_base8(parts[1]).value_or(0644U);
    error.clear();
    return std::pair<uid_t, unsigned int>{static_cast<uid_t>(*uid), mode};
}

} // namespace

StatusResult check_file_status(std::string_view path) {
    StatusResult result;
    const uid_t uid = sys::process::uid();

    // First try without doas.
    {
        std::string error;
        const std::optional<FileInfo> info = status_of(path, error);
        if (info) {
            result.info = *info;
            return result;
        }
    }

    // Try with doas.
    const std::string target(path);
    const bridge::Result exists = bridge::spawn_discard({"test", "-e", target});
    if (exists.status == 0) {
        const bridge::Result is_file = bridge::spawn_discard({"test", "-f", target});
        if (is_file.status != 0) {
            result.info.exists = true;
            result.info.is_directory = true;
            return result;
        }

        std::string error;
        const std::optional<std::pair<uid_t, unsigned int>> metadata =
            metadata_with_doas(target, error);
        if (!metadata) {
            result.error = error;
            return result;
        }

        result.info.exists = true;
        result.info.readable = true; // Assuming readable if doas can read it
        result.info.writable = (metadata->second & 0200U) != 0; // User write bit
        result.info.is_owned_by_user = metadata->first == uid;
        return result;
    }

    // File doesn't exist, check directory.
    const std::string dir = path::parent_of(path);
    const sys::fs::StatResult dir_stat = sys::fs::stat_follow(dir);
    if (!dir_stat.error) {
        const unsigned int mode = static_cast<unsigned int>(dir_stat.st.st_mode & 07777);
        if (dir_stat.st.st_uid == uid) {
            result.error = cannot_create_file_in_own_dir(path);
            return result;
        }
        if ((mode & 0222U) != 0 && dir_stat.st.st_uid == uid) {
            result.error = cannot_create_file_in_writable_dir(path);
            return result;
        }
    } else {
        const bridge::Result dir_exists = bridge::spawn_discard({"test", "-d", dir});
        if (dir_exists.status != 0) {
            result.error = no_directory_exists(dir);
            return result;
        }
    }

    return result;
}

namespace {

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
std::string run_editor(const std::string& editor, const std::string& file) {
    auto pipe = sys::fs::make_pipe();
    if (!pipe) {
        return invalid_editor(rust_io_msg(pipe.error()));
    }
    (void)sys::fs::set_close_on_exec(pipe->second.get());

    sys::flush_all();

    const std::expected<pid_t, std::error_code> child = sys::process::fork_child();
    if (!child) {
        return invalid_editor(rust_io_msg(child.error()));
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

    const sys::process::WaitResult waited = sys::process::wait_for(*child);
    const bool spawn_failed = got && static_cast<size_t>(*got) == sizeof(reported);
    if (spawn_failed) {
        return invalid_editor(rust_io_msg(std::error_code(reported, std::generic_category())));
    }
    if (waited.error) {
        return std::string(kEditorError);
    }
    if (WIFEXITED(waited.status) && WEXITSTATUS(waited.status) == 0) {
        return {};
    }
    return std::string(kEditorError);
}

// create_secure_temp_copy()
std::string create_secure_temp_copy(std::string_view file_path,
                                    std::string_view temp_dir,
                                    std::string& tmp_file,
                                    std::string& tmp_copy) {
    const std::optional<std::string> filename = path::file_name_of(file_path);
    if (!filename) {
        return invalid_editor(kInvalidFilePath);
    }
    const std::string copy_name = std::string(kCopyPrefix) + *filename;

    tmp_file = join_path(temp_dir, *filename);
    tmp_copy = join_path(temp_dir, copy_name);

    std::error_code error = sys::fs::write_file(tmp_file, "");
    if (error) {
        return rust_io_msg(error);
    }
    error = sys::fs::write_file(tmp_copy, "");
    if (error) {
        return rust_io_msg(error);
    }

    // Set secure permissions (0600).
    error = sys::fs::chmod_path(tmp_file, 0600);
    if (error) {
        return rust_io_msg(error);
    }
    error = sys::fs::chmod_path(tmp_copy, 0600);
    if (error) {
        return rust_io_msg(error);
    }
    return {};
}

// copy_original_content()
std::string
copy_original_content(const std::string& original, const std::string& tmp_file, bool use_doas) {
    if (use_doas) {
        const bridge::Result result = bridge::spawn_capture({"cat", original});
        if (result.status != 0) {
            return doas_cat_permission_denied();
        }
        const std::error_code error = sys::fs::write_file(tmp_file, result.out);
        if (error) {
            return rust_io_msg(error);
        }
        return {};
    }

    const auto copied = sys::fs::copy_file(original, tmp_file);
    if (!copied) {
        return rust_io_msg(copied.error());
    }
    return {};
}

// create_comparison_copy()
std::string create_comparison_copy(const std::string& tmp_file, const std::string& tmp_copy) {
    const auto copied = sys::fs::copy_file(tmp_file, tmp_copy);
    if (!copied) {
        return rust_io_msg(copied.error());
    }
    return {};
}

bool is_doas_config_file(std::string_view file_path) {
    return file_path.starts_with("/etc/doas") &&
           (file_path == "/etc/doas.conf" || file_path.starts_with("/etc/doas.d/"));
}

// validate_doas_config()
std::string validate_doas_config(const std::string& tmp_file, const std::string& editor) {
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
            return std::string(kInterrupted);
        }

        const std::string answer = lower_ascii(trim_ascii(*input));
        if (answer == "o") {
            break;
        }
        if (answer == "a") {
            return std::string(kUserAbort);
        }
        const std::string error = run_editor(editor, tmp_file);
        if (!error.empty()) {
            return error;
        }
    }

    return {};
}

// files_match()
std::optional<bool>
files_match(const std::string& first, const std::string& second, std::string& error) {
    const auto content1 = sys::fs::read_file(first);
    if (!content1) {
        error = rust_io_msg(content1.error());
        return std::nullopt;
    }
    const auto content2 = sys::fs::read_file(second);
    if (!content2) {
        error = rust_io_msg(content2.error());
        return std::nullopt;
    }
    return *content1 == *content2;
}

// write_file_back()
std::string
write_file_back(const std::string& tmp_file, const std::string& original, bool writable) {
    if (writable) {
        const auto copied = sys::fs::copy_file(tmp_file, original);
        if (!copied) {
            return rust_io_msg(copied.error());
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
            return three_incorrect_password_attempts();
        }
    }
    return three_incorrect_password_attempts();
}

} // namespace

std::string
process_file(std::string_view file_path, std::string_view editor, std::string_view tmp_dir) {
    // Check if path is a directory (ends with /).
    if (path::is_directory_path(file_path)) {
        return cannot_edit_directory(file_path);
    }

    std::string tmp_file;
    std::string tmp_copy;
    {
        const std::optional<std::string> filename = path::file_name_of(file_path);
        if (!filename) {
            return invalid_editor(kInvalidFilePath);
        }
    }

    const StatusResult status = check_file_status(file_path);
    if (!status.error.empty()) {
        return status.error;
    }
    const FileInfo info = status.info;

    const std::string create_error =
        create_secure_temp_copy(file_path, tmp_dir, tmp_file, tmp_copy);
    if (!create_error.empty()) {
        return create_error;
    }

    if (info.exists) {
        if (info.is_owned_by_user) {
            return cannot_edit_own_file(file_path);
        }
        if (info.is_directory) {
            return not_regular_file(file_path);
        }
        if (info.readable && info.writable) {
            return cannot_edit_readable_writable_file(file_path);
        }

        const bool use_doas = !info.readable;
        const std::string copy_error =
            copy_original_content(std::string(file_path), tmp_file, use_doas);
        if (!copy_error.empty()) {
            return copy_error;
        }

        const std::string comparison_error = create_comparison_copy(tmp_file, tmp_copy);
        if (!comparison_error.empty()) {
            return comparison_error;
        }
    }

    const std::string editor_error = run_editor(std::string(editor), tmp_file);
    if (!editor_error.empty()) {
        return editor_error;
    }

    if (is_doas_config_file(file_path)) {
        const std::string validation_error = validate_doas_config(tmp_file, std::string(editor));
        if (!validation_error.empty()) {
            return validation_error;
        }
    }

    std::string match_error;
    const std::optional<bool> same = files_match(tmp_file, tmp_copy, match_error);
    if (!same) {
        return match_error;
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
        sys::err_out("doasedit: using this program as root is not permitted\n");
        return 1;
    }

    const std::string editor = get_editor_command();

    const auto tmp_dir = sys::fs::make_temp_dir();
    if (!tmp_dir) {
        sys::err_out(std::format("doasedit: Error creating temporary directory: {}\n",
                                 rust_io_msg(tmp_dir.error())));
        return 1;
    }

    for (const std::string& file : parsed.files) {
        const std::string error = process_file(file, editor, *tmp_dir);
        if (!error.empty()) {
            // Like Rust's process::exit(1): the temporary directory is not
            // removed on this path.
            sys::err_out(std::format("doasedit: {}\n", error));
            return 1;
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(*tmp_dir, ignored);
    return 0;
}

} // namespace doasedit
