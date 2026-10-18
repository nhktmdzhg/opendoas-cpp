#ifndef DOAS_DOASEDIT_PATH_HPP
#define DOAS_DOASEDIT_PATH_HPP

#include <optional>
#include <string>
#include <string_view>

namespace doasedit::path {

// Rust's str::ends_with('/'), checked on the raw argument.
bool is_directory_path(std::string_view path);

// Rust's Path::parent() with the ".unwrap_or(Path::new("/"))" fallback used by
// get_parent_directory().
std::string parent_of(std::string_view path);

// Rust's Path::file_name() + to_str(): nullopt for "/", ".", "..", "" (a path
// without a final component) and for names that are not valid UTF-8.
std::optional<std::string> file_name_of(std::string_view path);

} // namespace doasedit::path

#endif // DOAS_DOASEDIT_PATH_HPP
