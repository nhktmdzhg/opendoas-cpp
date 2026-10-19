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
        const auto pw = sys::pw_by_name(user);
        if (!pw) {
            // getpwnam(3) reports a missing entry without touching errno, so the
            // original printed "getpwnam: Success" there; an errno keeps its text.
            sys::die(1,
                     pw.error().is(sys::Err::kNotFound) ? sys::fail_errno(0) : pw.error(),
                     "getpwnam");
        }

        // The hash is copied out of libc's static storage: crypt(3) is called
        // right afterwards and the original kept a pointer into
        // getpwnam(3)/getspnam(3) storage there.
        std::string hash = pw->passwd;
        if (hash == "x") {
            const auto sp = sys::shadow_hash(user);
            if (!sp) {
                // getspnam(3) returned NULL: the original reported this as an
                // authentication failure (a NULL sp_pwdp would have been
                // dereferenced).
                sys::die(1, "Authentication failed");
            }
            hash = *sp;
        } else if (hash.empty() || hash[0] != '*') {
            sys::die(1, "Authentication failed");
        }

        const std::string myname(user);
        const std::string challenge = password_prompt(myname, sys::process::hostname());

        sys::Secret rbuf(1024);
        const auto response = sys::readpassphrase(challenge, rbuf.span(), sys::kRppRequireTty);
        if (!response) {
            if (response.error().is(sys::Err::kNotATerminal)) {
                sys::log(LOG_AUTHPRIV | LOG_NOTICE, std::format("tty required for {}", myname));
                sys::die(1, "a tty is required");
            }
            sys::die(1, response.error(), "readpassphrase");
        }

        const std::optional<std::string> encrypted = sys::crypt_hash(*response, hash);
        if (!encrypted) {
            explicit_bzero(rbuf.data(), rbuf.span().size());
            sys::die(1, "Authentication failed");
        }
        explicit_bzero(rbuf.data(), rbuf.span().size());
        if (*encrypted != hash) {
            sys::log(LOG_AUTHPRIV | LOG_NOTICE, std::format("failed auth for {}", myname));
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
