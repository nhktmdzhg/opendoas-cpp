#ifndef DOAS_DOASEDIT_DOASEDIT_HPP
#define DOAS_DOASEDIT_DOASEDIT_HPP

#include <string>
#include <string_view>

namespace doasedit {

// Port of FileInfo from file_handler.rs.
struct FileInfo {
    bool exists = false;
    bool readable = false;
    bool writable = false;
    bool is_directory = false;
    bool is_owned_by_user = false;
};

struct StatusResult {
    FileInfo info;
    std::string error; // empty on success
};

// Port of check_file_status().
StatusResult check_file_status(std::string_view path);

// Port of process_file(); empty string means success, otherwise the message
// printed as "doasedit: <message>".
std::string
process_file(std::string_view file_path, std::string_view editor, std::string_view tmp_dir);

// Applet entry point: returns the process status.
int main(int argc, char* const argv[]);

} // namespace doasedit

#endif // DOAS_DOASEDIT_DOASEDIT_HPP
