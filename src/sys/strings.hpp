#ifndef DOAS_SYS_STRINGS_HPP
#define DOAS_SYS_STRINGS_HPP

#include <cstddef>
#include <string_view>
#include <vector>

namespace sys {

// Bounded string builder with the exact semantics of strlcpy(3)/strlcat(3) from
// libopenbsd. Used for the LINE_MAX command log line and for PATH_MAX path
// construction, where the truncation behaviour is part of the logic.
class BoundedString {
  public:
    explicit BoundedString(size_t cap);

    // strlcpy(): replace the content, truncating to cap - 1 bytes.
    void assign(std::string_view src);

    // strlcat(): append, truncating to cap - 1 bytes. Returns false when the
    // result (dst len + src len) is >= cap, i.e. when truncation happened.
    [[nodiscard]] bool append(std::string_view src);

    [[nodiscard]] std::string_view view() const;

  private:
    std::vector<char> buf_;
    size_t len_ = 0;
};

// Strict UTF-8 validation (rejects overlong forms, surrogates and > U+10FFFF),
// matching Rust's str::from_utf8 for the cases doasedit relies on.
[[nodiscard]] bool utf8_valid(std::string_view s);

// Comparison for secret material: the loop walks the longer of the two inputs
// whatever the contents, so the position of the first difference cannot be timed.
// Used for the shadow password hash comparison.
[[nodiscard]] bool constant_time_equals(std::string_view a, std::string_view b);

} // namespace sys

#endif // DOAS_SYS_STRINGS_HPP
