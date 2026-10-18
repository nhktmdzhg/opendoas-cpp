#include "doasedit/path.hpp"

#include "sys/strings.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace doasedit::path {

namespace {

// Component view of a path, close to Rust's std::path::Path::components():
// repeated separators and interior/trailing "." components are normalised away,
// ".." is kept as a component.
struct Components {
    bool root = false;
    std::vector<std::string_view> parts;
};

Components split(std::string_view path) {
    Components out;

    size_t i = 0;
    if (!path.empty() && path[0] == '/') {
        out.root = true;
        while (i < path.size() && path[i] == '/') {
            i++;
        }
    }

    while (i < path.size()) {
        const size_t start = i;
        while (i < path.size() && path[i] != '/') {
            i++;
        }
        const std::string_view part = path.substr(start, i - start);
        while (i < path.size() && path[i] == '/') {
            i++;
        }
        if (part.empty() || part == ".") {
            continue;
        }
        out.parts.push_back(part);
    }

    return out;
}

} // namespace

bool is_directory_path(std::string_view path) {
    return path.ends_with('/');
}

std::string parent_of(std::string_view path) {
    const Components components = split(path);

    if (components.parts.empty()) {
        // "/" has no parent in Rust and the caller falls back to "/"; "", "." and
        // ".." never reach here because those paths exist.
        return "/";
    }

    if (components.parts.size() == 1) {
        return components.root ? "/" : "";
    }

    // Rust returns a prefix of the original string, so rebuild it from the text.
    size_t end = path.size();
    while (end > 0 && path[end - 1] == '/') {
        end--;
    }
    size_t start = end;
    while (start > 0 && path[start - 1] != '/') {
        start--;
    }
    while (start > 0 && path[start - 1] == '/') {
        start--;
    }
    if (start == 0) {
        return components.root ? "/" : "";
    }
    return std::string(path.substr(0, start));
}

std::optional<std::string> file_name_of(std::string_view path) {
    const Components components = split(path);
    if (components.parts.empty()) {
        return std::nullopt;
    }
    // Rust's Path::file_name() is None when the path terminates in "..".
    if (components.parts.back() == "..") {
        return std::nullopt;
    }
    const std::string_view name = components.parts.back();
    if (!sys::utf8_valid(name)) {
        // to_str() fails on a name that is not valid UTF-8.
        return std::nullopt;
    }
    return std::string(name);
}

} // namespace doasedit::path
