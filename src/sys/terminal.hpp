#ifndef DOAS_SYS_TERMINAL_HPP
#define DOAS_SYS_TERMINAL_HPP

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sys {

// readpassphrase(3) flags with the same values as libopenbsd/readpassphrase.c.
enum RppFlags : int {
    kRppEchoOff = 0x00,
    kRppEchoOn = 0x01,
    kRppRequireTty = 0x02,
    kRppForceLower = 0x04,
    kRppForceUpper = 0x08,
    kRppSevenBit = 0x10,
    kRppStdin = 0x20,
};

bool is_a_tty(int fd);

// ttyname(3) with a leading "/dev/" stripped (as doas did before storing it in
// PAM_TTY).
std::optional<std::string> tty_name(int fd);

// Port of libopenbsd/readpassphrase.c. The passphrase is stored inside buf,
// which must outlive the returned view. nullopt means failure with errno set.
std::optional<std::string_view>
readpassphrase(std::string_view prompt, std::span<char> buf, int flags);

} // namespace sys

#endif // DOAS_SYS_TERMINAL_HPP
