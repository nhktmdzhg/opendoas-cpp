/*
 * Copyright (c) 2015 Nathan Holstein <nathan.holstein@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

// Port of pam.c. The fork/session structure is kept exactly: pamauth() forks,
// the child returns to the caller (which continues the doas flow) and the parent
// waits for the child and exits with its status through watch_session().
#include "doas/auth.hpp"
#include "doas/timestamp.hpp"
#include "sys/error.hpp"
#include "sys/fs.hpp"
#include "sys/io.hpp"
#include "sys/process.hpp"
#include "sys/secret.hpp"
#include "sys/terminal.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <security/pam_appl.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <vector>

namespace doas {

namespace {

volatile sig_atomic_t g_caught_signal = 0;
std::string g_doas_prompt;

void catch_signal(int sig) {
    g_caught_signal = sig;
}

struct FreeDeleter {
    void operator()(char* ptr) const {
        std::free(ptr);
    }
};

using RespPtr = std::unique_ptr<char, FreeDeleter>;

// Owns the pam_handle_t* produced by pam_start(3). All PAM calls take a raw
// handle because that is the libpam ABI; the handle itself is only ever
// destroyed through end().
class PamSession {
  public:
    int start(const char* service, const char* user, const struct pam_conv* conv) {
        return pam_start(service, user, conv, &handle_);
    }

    [[nodiscard]] pam_handle_t* raw() const {
        return handle_;
    }

    void end(int status) {
        if (handle_ != nullptr) {
            pam_handle_t* handle = handle_;
            handle_ = nullptr;
            (void)pam_end(handle, status);
        }
    }

  private:
    pam_handle_t* handle_ = nullptr;
};

PamSession g_pam;

// pamprompt(): the response string is malloc(3)ed because libpam takes
// ownership of it (and frees it with free(3)).
RespPtr pam_prompt(const char* msg, bool echo_on, int& ret) {
    const int flags = sys::kRppRequireTty | (echo_on ? sys::kRppEchoOn : sys::kRppEchoOff);

    // overwrite default prompt if it matches "Password:[ ]"
    const std::string_view message(msg);
    std::string_view prompt;
    if (message.starts_with("Password:") &&
        (message.size() == 9 || (message.size() == 10 && message[9] == ' '))) {
        prompt = g_doas_prompt;
    } else {
        prompt = message;
    }

    sys::Secret buf(PAM_MAX_RESP_SIZE);
    const auto pass = sys::readpassphrase(prompt, buf.span(), flags);

    RespPtr response;
    if (!pass) {
        ret = PAM_CONV_ERR;
        return response;
    }
    response.reset(::strdup(std::string(*pass).c_str()));
    if (!response) {
        ret = PAM_BUF_ERR;
        return response;
    }
    ret = PAM_SUCCESS;
    return response;
}

int pam_conversation(int nmsgs,
                     const struct pam_message** msgs,
                     struct pam_response** rsps,
                     void* data) {
    (void)data;

    const auto count = static_cast<size_t>(nmsgs);
    auto responses = std::make_unique<struct pam_response[]>(count);
    std::vector<RespPtr> owned(count);

    for (size_t i = 0; i < count; i++) {
        const int style = msgs[i]->msg_style;
        switch (style) {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON: {
            int ret = PAM_SUCCESS;
            owned[i] = pam_prompt(msgs[i]->msg, style == PAM_PROMPT_ECHO_ON, ret);
            responses[i].resp = owned[i].get();
            if (ret != PAM_SUCCESS) {
                goto fail;
            }
            break;
        }

        case PAM_ERROR_MSG:
        case PAM_TEXT_INFO:
            if (!sys::err_out_checked(std::format("{}\n", msgs[i]->msg))) {
                goto fail;
            }
            break;

        default:
            sys::die(1, std::format("invalid PAM msg_style {}", style));
        }
    }

    *rsps = responses.release();
    for (RespPtr& response : owned) {
        (void)response.release();
    }
    return PAM_SUCCESS;

fail:
    // overwrite and free response buffers
    for (size_t i = 0; i < count; i++) {
        if (owned[i]) {
            explicit_bzero(owned[i].get(), std::strlen(owned[i].get()));
            owned[i].reset();
        }
        responses[i].resp = nullptr;
    }
    return PAM_CONV_ERR;
}

void pam_cleanup(int ret, bool sess, bool cred) {
    if (sess) {
        ret = pam_close_session(g_pam.raw(), 0);
        if (ret != PAM_SUCCESS) {
            sys::die(1, std::format("pam_close_session: {}", pam_strerror(g_pam.raw(), ret)));
        }
    }
    if (cred) {
        ret = pam_setcred(g_pam.raw(), PAM_DELETE_CRED | PAM_SILENT);
        if (ret != PAM_SUCCESS) {
            sys::warn(std::format("pam_setcred(?, PAM_DELETE_CRED | PAM_SILENT): {}",
                                  pam_strerror(g_pam.raw(), ret)));
        }
    }
    g_pam.end(ret);
}

void watch_session(pid_t child, bool sess, bool cred) {
    struct sigaction oldact{};
    // Declared before the first goto so that jumping to "close" stays legal.
    sys::Result<struct sigaction> term = std::unexpected(sys::fail_errno(0));
    int status = 1;

    const auto masked = sys::process::block_all_signals();
    if (!masked) {
        sys::warn(masked.error(), "failed to block signals");
        g_caught_signal = 1;
        goto close;
    }

    {
        // setup signal handler, then unblock SIGTERM, SIGALRM and SIGTSTP to
        // catch them (same order as the original: the three sigaddset(3) calls
        // come before sigaction(3))
        sigset_t sigs;
        ::sigemptyset(&sigs);
        const bool add_failed = ::sigaddset(&sigs, SIGTERM) != 0 ||
                                ::sigaddset(&sigs, SIGALRM) != 0 ||
                                ::sigaddset(&sigs, SIGTSTP) != 0;
        if (!add_failed) {
            term = sys::process::install_handler(SIGTERM, catch_signal);
        }
        if (add_failed || !term) {
            // sigaddset(3) reports EINVAL through errno; sigaction(3) through the
            // error code. Both print the same diagnostic in the original.
            sys::warn(add_failed ? sys::fail_errno(errno) : term.error(),
                      "failed to set signal handler");
            g_caught_signal = 1;
            goto close;
        }
        if (sys::Status unblocked = sys::process::unblock_signals(sigs); !unblocked) {
            sys::warn(unblocked.error(), "failed to set signal handler");
            g_caught_signal = 1;
            goto close;
        }
        oldact = *term;
    }

    // wait for child to be terminated
    {
        const auto waited = sys::process::wait_for(child);
        if (waited) {
            status = waited->shell_code();
            if (waited->signaled()) {
                sys::err_out(std::format("{}{}\n",
                                         strsignal(waited->term_signal()),
                                         waited->core_dumped() ? " (core dumped)" : ""));
            }
        } else if (g_caught_signal != 0) {
            status = g_caught_signal + 128;
        } else {
            status = 1;
        }
    }

close:
    if (g_caught_signal != 0 && child != static_cast<pid_t>(-1)) {
        sys::err_out("\nSession terminated, killing shell\n");
        sys::process::send_signal(child, SIGTERM);
    }

    pam_cleanup(PAM_SUCCESS, sess, cred);

    if (g_caught_signal != 0) {
        if (child != static_cast<pid_t>(-1)) {
            // kill child
            sys::process::sleep_seconds(2);
            sys::process::send_signal(child, SIGKILL);
            sys::err_out(" ...killed.\n");
        }

        // unblock cached signal and resend
        (void)sys::process::restore_handler(SIGTERM, oldact);
        if (g_caught_signal != SIGTERM) {
            g_caught_signal = SIGKILL;
        }
        sys::process::send_signal(sys::process::pid(), g_caught_signal);
    }

    std::exit(status);
}

} // namespace

void pamauth(std::string_view target_user,
             std::string_view self_user,
             bool interactive,
             bool nopass,
             bool persist) {
    static const struct pam_conv conv = {pam_conversation, nullptr};

#if defined(DOAS_PERSIST)
    std::optional<doas::OpenedTimestamp> timestamp;
#else
    (void)persist;
#endif

    if (target_user.empty() || self_user.empty()) {
        sys::die(1, "Authentication failed");
    }

    const std::string myname(self_user);
    const std::string target(target_user);

    int ret = g_pam.start(kPamServiceName, myname.c_str(), &conv);
    if (ret != PAM_SUCCESS) {
        sys::die(1, std::format(R"(pam_start("{}", "{}", ?, ?): failed)", kPamServiceName, myname));
    }

    ret = pam_set_item(g_pam.raw(), PAM_RUSER, myname.c_str());
    if (ret != PAM_SUCCESS) {
        sys::warn(sys::fail_errno(errno),
                  std::format("pam_set_item(?, PAM_RUSER, \"{}\"): {}",
                              pam_strerror(g_pam.raw(), ret),
                              myname));
    }

    if (sys::is_a_tty(0)) {
        const std::optional<std::string> ttydev = sys::tty_name(0);
        if (ttydev) {
            ret = pam_set_item(g_pam.raw(), PAM_TTY, ttydev->c_str());
            if (ret != PAM_SUCCESS) {
                sys::warn(sys::fail_errno(errno),
                          std::format("pam_set_item(?, PAM_TTY, \"{}\"): {}",
                                      *ttydev,
                                      pam_strerror(g_pam.raw(), ret)));
            }
        }
    }

    bool sess = false;
    bool cred = false;

#if defined(DOAS_PERSIST)
    if (persist) {
        auto opened = doas::timestamp_open(5 * 60);
        if (opened) {
            timestamp = std::move(*opened);
        }
    }
    if (timestamp && timestamp->valid) {
        nopass = true;
    }
#endif

    if (!nopass) {
        if (!interactive) {
            sys::die(1, "Authentication required");
        }

        // doas style prompt for pam
        g_doas_prompt = password_prompt(myname, sys::process::hostname());

        ret = pam_authenticate(g_pam.raw(), 0);
        if (ret != PAM_SUCCESS) {
            pam_cleanup(ret, sess, cred);
            sys::log(LOG_AUTHPRIV | LOG_NOTICE, std::format("failed auth for {}", myname));
            sys::die(1, "Authentication failed");
        }
    }

    ret = pam_acct_mgmt(g_pam.raw(), 0);
    if (ret == PAM_NEW_AUTHTOK_REQD) {
        ret = pam_chauthtok(g_pam.raw(), PAM_CHANGE_EXPIRED_AUTHTOK);
    }

    // account not valid or changing the auth token failed
    if (ret != PAM_SUCCESS) {
        pam_cleanup(ret, sess, cred);
        sys::log(LOG_AUTHPRIV | LOG_NOTICE, std::format("failed auth for {}", myname));
        sys::die(1, "Authentication failed");
    }

    // set PAM_USER to the user we want to be
    ret = pam_set_item(g_pam.raw(), PAM_USER, target.c_str());
    if (ret != PAM_SUCCESS) {
        sys::warn(sys::fail_errno(errno),
                  std::format("pam_set_item(?, PAM_USER, \"{}\"): {}",
                              target,
                              pam_strerror(g_pam.raw(), ret)));
    }

    ret = pam_setcred(g_pam.raw(), PAM_REINITIALIZE_CRED);
    if (ret != PAM_SUCCESS) {
        sys::warn(sys::fail_errno(errno),
                  std::format("pam_setcred(?, PAM_REINITIALIZE_CRED): {}",
                              pam_strerror(g_pam.raw(), ret)));
    } else {
        cred = true;
    }

    // open session
    ret = pam_open_session(g_pam.raw(), 0);
    if (ret != PAM_SUCCESS) {
        sys::die(1, std::format("pam_open_session: {}", pam_strerror(g_pam.raw(), ret)));
    }
    sess = true;

    const auto child = sys::process::fork_child();
    if (!child) {
        pam_cleanup(PAM_ABORT, sess, cred);
        sys::die(1, child.error(), "fork");
    }

    // return as child
    if (*child == 0) {
#if defined(DOAS_PERSIST)
        timestamp.reset();
#endif
        return;
    }

#if defined(DOAS_PERSIST)
    if (timestamp) {
        (void)doas::timestamp_set(timestamp->fd, 5 * 60);
        timestamp.reset();
    }
#endif
    watch_session(*child, sess, cred);
}

} // namespace doas
