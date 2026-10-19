/*
 * Copyright (c) 2020 Duncan Overbruck <mail@duncano.de>
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

// Port of shadow.c (the shadow based authentication provider).
#include "doas/auth.hpp"
#include "doas/timestamp.hpp"
#include "sys/error.hpp"
#include "sys/process.hpp"
#include "sys/secret.hpp"
#include "sys/strings.hpp"
#include "sys/terminal.hpp"
#include "sys/users.hpp"

#include <cerrno>
#include <crypt.h>
#include <cstring> // IWYU pragma: keep
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <syslog.h>
#include <unistd.h>

namespace doas {

namespace {

// How long a failed attempt is delayed before it is reported. A correct password
// returns immediately; a wrong one costs a second, which caps guessing at one
// attempt per second. PAM runs its own timing (pam_faildelay / pam_faillock), so
// this only applies to the shadow provider.
constexpr unsigned int kFailedAuthDelaySeconds = 1;

// Copies `text` into an already sized wiped buffer and wipes the source, for values
// that must not survive on the heap (the shadow hash).
void wipe_and_copy(std::string& text, sys::Secret& secret) {
    if (!text.empty()) {
        std::memcpy(secret.data(), text.data(), text.size());
    }
    explicit_bzero(text.data(), text.size());
}

} // namespace

void shadow_auth(std::string_view user, bool persist) {
#if defined(DOAS_PERSIST)
    std::optional<doas::OpenedTimestamp> timestamp;

    if (persist) {
        auto opened = doas::timestamp_open(5 * 60);
        if (opened) {
            timestamp = std::move(*opened);
        }
    }
    // The original jumps straight to "good" when the timestamp is still valid,
    // i.e. it skips authentication entirely.
    const bool already_authenticated = timestamp && timestamp->valid;
#else
    (void)persist;
#endif

#if defined(DOAS_PERSIST)
    if (!already_authenticated)
#endif
    {
        const std::string myname(user);

        auto pw = sys::pw_by_name(user);
        if (!pw) {
            // getpwnam(3) reports a missing entry without touching errno, so the
            // original printed "getpwnam: Success" there; an errno keeps its text.
            sys::die(1,
                     pw.error().is(sys::Err::kNotFound) ? sys::fail_errno(0) : pw.error(),
                     "getpwnam");
        }

        // The verifier is copied out of libc's storage and out of the passwd entry
        // into a wiped buffer: a passwd entry is not worth keeping in memory.
        std::string verifier_text = pw->passwd;
        explicit_bzero(pw->passwd.data(), pw->passwd.size());
        if (verifier_text == "x") {
            auto sp = sys::shadow_hash(user);
            if (!sp) {
                // getspnam(3) returned NULL: the original reported this as an
                // authentication failure (a NULL sp_pwdp would have been
                // dereferenced).
                sys::die(1, "Authentication failed");
            }
            verifier_text = *sp;
            explicit_bzero(sp->data(), sp->size());
        } else if (verifier_text.empty() || verifier_text[0] != '*') {
            explicit_bzero(verifier_text.data(), verifier_text.size());
            sys::die(1, "Authentication failed");
        }

        sys::Secret verifier(verifier_text.size() + 1);
        wipe_and_copy(verifier_text, verifier);
        const std::string_view setting(verifier.data());

        const std::string challenge = password_prompt(myname, sys::process::hostname());

        // Everything that touches the password lives in this scope. Its Secret
        // destructor wipes the buffer, and the failures below call exit() (which
        // does not run destructors), so they are reported only after the scope ends.
        bool matched = false;
        bool prompt_failed = false;
        sys::Error prompt_error;
        {
            sys::Secret rbuf(1024);
            const auto response = sys::readpassphrase(challenge, rbuf.span(), sys::kRppRequireTty);
            if (!response) {
                prompt_failed = true;
                prompt_error = response.error();
            } else {
                std::optional<std::string> encrypted = sys::crypt_hash(*response, setting);
                if (encrypted) {
                    matched = sys::constant_time_equals(*encrypted, setting);
                    explicit_bzero(encrypted->data(), encrypted->size());
                }
            }
        }

        if (prompt_failed) {
            if (prompt_error.is(sys::Err::kNotATerminal)) {
                sys::log(LOG_AUTHPRIV | LOG_NOTICE, std::format("tty required for {}", myname));
                sys::die(1, "a tty is required");
            }
            sys::die(1, prompt_error, "readpassphrase");
        }

        if (!matched) {
            sys::log(LOG_AUTHPRIV | LOG_NOTICE, std::format("failed auth for {}", myname));
            // Slow down guessing: a wrong password costs a second before the failure
            // is reported, while a correct one returns immediately.
            sys::process::sleep_seconds(kFailedAuthDelaySeconds);
            sys::die(1, "Authentication failed");
        }
    }

#if defined(DOAS_PERSIST)
    if (timestamp) {
        (void)doas::timestamp_set(timestamp->fd, 5 * 60);
        timestamp.reset();
    }
#endif
}

} // namespace doas
