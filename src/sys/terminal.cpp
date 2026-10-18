// Port of libopenbsd/readpassphrase.c (OpenBSD readpassphrase(3), originally
// from openssh). The algorithm is kept identical: same termios dance, same
// signal save/restore, same restart-after-SIGTSTP logic.
#include "sys/terminal.hpp"

#include "sys/process.hpp"

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace {

volatile sig_atomic_t g_signo[_NSIG];

void rpp_handler(int sig) {
    g_signo[sig] = 1;
}

#if defined(TCSASOFT)
// Platforms without TCSASOFT treat the flag as a no-op, exactly like the
// "#define TCSASOFT 0" fallback of the original.
constexpr int kTcsasoft = TCSASOFT;
#else
constexpr int kTcsasoft = 0;
#endif

constexpr const char* kTtyPath = "/dev/tty";

} // namespace

namespace sys {

bool is_a_tty(int fd) {
    return ::isatty(fd) == 1;
}

std::optional<std::string> tty_name(int fd) {
    const char* name = ::ttyname(fd);
    if (name == nullptr) {
        return std::nullopt;
    }
    std::string out(name);
    if (out.starts_with("/dev/")) {
        out.erase(0, 5);
    }
    return out;
}

Result<std::string_view> readpassphrase(std::string_view prompt, std::span<char> buf, int flags) {
    if (buf.empty()) {
        return std::unexpected(fail_errno(EINVAL));
    }

    ssize_t nr = -1;
    int input = -1;
    int output = -1;
    int save_errno = 0;
    bool need_restart = false;
    struct termios term{};
    struct termios oterm{};

restart:
    for (volatile sig_atomic_t& state : g_signo) {
        state = 0;
    }
    // The original also reset nr and save_errno here; both are always reassigned
    // before being read, so the stores were dead.
    need_restart = false;

    // Read and write to /dev/tty when available; otherwise use stdin/stderr
    // unless a tty is required.
    if ((flags & kRppStdin) != 0 || (input = output = ::open(kTtyPath, O_RDWR)) == -1) {
        if ((flags & kRppRequireTty) != 0) {
            return std::unexpected(fail(Err::kNotATerminal));
        }
        input = STDIN_FILENO;
        output = STDERR_FILENO;
    }

    // Turn off echo if possible. This must happen before installing the signal
    // handlers: using a tty we are not the foreground process group of would
    // raise SIGTTOU.
    if (input != STDIN_FILENO && ::tcgetattr(input, &oterm) == 0) {
        term = oterm;
        if ((flags & kRppEchoOn) == 0) {
            term.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHONL);
        }
#ifdef VSTATUS
        if (term.c_cc[VSTATUS] != _POSIX_VDISABLE) {
            term.c_cc[VSTATUS] = _POSIX_VDISABLE;
        }
#endif
        (void)::tcsetattr(input, TCSAFLUSH | kTcsasoft, &term);
    } else {
        std::memset(&term, 0, sizeof(term));
        term.c_lflag |= ECHO;
        std::memset(&oterm, 0, sizeof(oterm));
        oterm.c_lflag |= ECHO;
    }

    // Catch the signals that would otherwise leave the shell with echo disabled.
    const Result<struct sigaction> savealrm = process::install_handler(SIGALRM, rpp_handler);
    const Result<struct sigaction> savehup = process::install_handler(SIGHUP, rpp_handler);
    const Result<struct sigaction> saveint = process::install_handler(SIGINT, rpp_handler);
    const Result<struct sigaction> savepipe = process::install_handler(SIGPIPE, rpp_handler);
    const Result<struct sigaction> savequit = process::install_handler(SIGQUIT, rpp_handler);
    const Result<struct sigaction> saveterm = process::install_handler(SIGTERM, rpp_handler);
    const Result<struct sigaction> savetstp = process::install_handler(SIGTSTP, rpp_handler);
    const Result<struct sigaction> savettin = process::install_handler(SIGTTIN, rpp_handler);
    const Result<struct sigaction> savettou = process::install_handler(SIGTTOU, rpp_handler);

    if ((flags & kRppStdin) == 0) {
        (void)::write(output, prompt.data(), prompt.size());
    }

    char* cursor = buf.data();
    char* const end = buf.data() + buf.size() - 1;
    char ch = 0;
    while ((nr = ::read(input, &ch, 1)) == 1 && ch != '\n' && ch != '\r') {
        if (cursor < end) {
            if ((flags & kRppSevenBit) != 0) {
                ch = static_cast<char>(ch & 0x7f);
            }
            if (std::isalpha(static_cast<unsigned char>(ch)) != 0) {
                if ((flags & kRppForceLower) != 0) {
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                if ((flags & kRppForceUpper) != 0) {
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                }
            }
            *cursor++ = ch;
        }
    }
    *cursor = '\0';
    save_errno = errno;
    if ((term.c_lflag & ECHO) == 0) {
        (void)::write(output, "\n", 1);
    }

    // Restore the old terminal settings and signal handlers. The original used
    // memcmp(3) here; term is a copy of oterm with at most c_lflag (echo flags)
    // and c_cc[VSTATUS] modified, so comparing those members is equivalent and
    // avoids comparing structure padding.
    const bool termios_changed = term.c_lflag != oterm.c_lflag;
#ifdef VSTATUS
    const bool vstatus_changed = term.c_cc[VSTATUS] != oterm.c_cc[VSTATUS];
#else
    constexpr bool vstatus_changed = false;
#endif
    if (termios_changed || vstatus_changed) {
        const sig_atomic_t sigttou = g_signo[SIGTTOU];

        // Ignore SIGTTOU raised because we are not the foreground process group.
        while (::tcsetattr(input, TCSAFLUSH | kTcsasoft, &oterm) == -1 && errno == EINTR &&
               g_signo[SIGTTOU] == 0) {
            continue;
        }
        g_signo[SIGTTOU] = sigttou;
    }
    // Restore only the handlers that were installed successfully. The original
    // restored the (uninitialised) saved action unconditionally, which is
    // undefined behaviour when sigaction(2) had failed.
    if (savealrm) {
        (void)process::restore_handler(SIGALRM, *savealrm);
    }
    if (savehup) {
        (void)process::restore_handler(SIGHUP, *savehup);
    }
    if (saveint) {
        (void)process::restore_handler(SIGINT, *saveint);
    }
    if (savequit) {
        (void)process::restore_handler(SIGQUIT, *savequit);
    }
    if (savepipe) {
        (void)process::restore_handler(SIGPIPE, *savepipe);
    }
    if (saveterm) {
        (void)process::restore_handler(SIGTERM, *saveterm);
    }
    if (savetstp) {
        (void)process::restore_handler(SIGTSTP, *savetstp);
    }
    if (savettin) {
        (void)process::restore_handler(SIGTTIN, *savettin);
    }
    if (savettou) {
        (void)process::restore_handler(SIGTTOU, *savettou);
    }
    if (input != STDIN_FILENO) {
        (void)::close(input);
    }

    // If we were interrupted by a signal, resend it to ourselves now that the
    // signal handlers are restored.
    for (int i = 0; i < _NSIG; i++) {
        if (g_signo[i] != 0) {
            (void)::kill(::getpid(), i);
            switch (i) {
            case SIGTSTP:
            case SIGTTIN:
            case SIGTTOU:
                need_restart = true;
                break;
            default:
                break;
            }
        }
    }
    if (need_restart) {
        goto restart;
    }

    if (nr == -1) {
        return std::unexpected(fail_errno(save_errno));
    }
    return std::string_view(buf.data(), static_cast<size_t>(cursor - buf.data()));
}

} // namespace sys
