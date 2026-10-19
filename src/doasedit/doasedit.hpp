#ifndef DOAS_DOASEDIT_DOASEDIT_HPP
#define DOAS_DOASEDIT_DOASEDIT_HPP

#include "sys/error.hpp"

#include <string>
#include <string_view>
#include <system_error>

namespace doasedit {

// Failure kinds of the applet (port of error.rs). Everything that comes from a
// system call keeps its errno code instead, and prose() renders it like Rust's
// io::Error ("<strerror> (os error <N>)").
enum class Err {
    kRootUser,         // using this program as root is not permitted
    kNoDoas,           // unreachable since the doas subprocess was removed (message kept)
    kEditorInvalid,    // invalid editor command: '<subject>'
    kEditorFailed,     // editor exited with non-zero status
    kCannotEditDir,    // <subject>: cannot edit directories
    kNotRegular,       // <subject>: not a regular file
    kOwnFile,          // <subject>: editing your own files is not permitted
    kOwnDir,           // <subject>: creating files in your own directory is not permitted
    kWritableDir,      // <subject>: creating files in a user-writable directory is not permitted
    kNoDirectory,      // <subject>: no such directory
    kReadableWritable, // <subject>: editing user-readable and -writable files is not permitted
    kDoasCat,          // you are not permitted to call 'doas cat'
    kThreeAttempts,    // 3 incorrect password attempts
    kInterrupted,      // interrupted
    kUserAbort,        // aborted by user
    kInvalidPath,      // invalid editor command: 'Invalid file path'
    kTempDirectory,    // Error creating temporary directory: <subject>
};

const std::error_category& category(); // name() == "doasedit"
std::error_code make_error_code(Err value) noexcept;

// Renders the exact message of the Rust implementation, byte for byte.
[[nodiscard]] std::string prose(const sys::Error& error);

// Port of FileInfo from file_handler.rs.
struct FileInfo {
    bool exists = false;
    bool readable = false;
    bool writable = false;
    bool is_directory = false;
    bool is_owned_by_user = false;
};

// Port of check_file_status().
[[nodiscard]] sys::Result<FileInfo> check_file_status(std::string_view path);

// Port of process_file(). The error carries the kind and the path needed by
// prose().
[[nodiscard]] sys::Status
process_file(std::string_view file_path, std::string_view editor, std::string_view tmp_dir);

// Applet entry point: returns the process status.
int main(int argc, char* const argv[]);

} // namespace doasedit

namespace std {
template <>
struct is_error_code_enum<doasedit::Err> : true_type {};
} // namespace std

#endif // DOAS_DOASEDIT_DOASEDIT_HPP
