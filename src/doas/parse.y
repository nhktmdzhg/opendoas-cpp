/* $OpenBSD: parse.y,v 1.10 2015/07/24 06:36:42 zhuk Exp $ */
/*
 * Copyright (c) 2015 Ted Unangst <tedu@openbsd.org>
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

/*
 * Grammar and lexer are the ones from the original parse.y. Only the plumbing
 * changed: semantic values are C++ types (bison's C++ skeleton with
 * api.value.type variant), so there are no raw owning pointers and no union of
 * PODs. The lexer keeps the original counters (lineno/colno), buffer handling
 * and diagnostics byte for byte.
 *
 * NOTE: this file is intentionally not clang-formatted (see CMakeLists.txt).
 */

%language "C++"
%skeleton "lalr1.cc"
%define api.namespace {doas}
%define api.parser.class {ConfigParser}
%define api.value.type variant
%define api.token.constructor
%define parse.error simple

%code requires {
#include <optional>
#include <string>
#include <vector>

namespace doas {

struct ActionValue {
    int action = 0;
    unsigned options = 0;
    std::optional<std::vector<std::string>> envlist;
};

struct OptionsValue {
    unsigned options = 0;
    std::optional<std::vector<std::string>> envlist;
};

struct CmdValue {
    std::optional<std::string> cmd;
    std::optional<std::vector<std::string>> cmdargs;
};

} // namespace doas
}

%code {
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cstring>
#include <format>
#include <string_view>

#include "doas/config.hpp"
#include "sys/error.hpp"
#include "sys/io.hpp"

namespace {

// Lexer state, replacing the globals yyfp/yylval of the original lexer. lineno
// and colno have the same meaning and are updated at exactly the same points.
struct LexerState {
    std::FILE* fp = nullptr;
    int lineno = 0;
    int colno = 0;
};

LexerState g_lex;

doas::ConfigParser::symbol_type yylex();
void config_error(std::string_view msg);

} // namespace
}

%token TNL TLBRACE TRBRACE
%token TPERMIT TDENY TAS TCMD TARGS
%token TNOPASS TNOLOG TPERSIST TKEEPENV TSETENV
%token <std::string> TSTRING

%nterm <std::vector<std::string>> strlist
%nterm <doas::ActionValue> action
%nterm <doas::OptionsValue> options option
%nterm <std::string> ident
%nterm <std::optional<std::string>> target
%nterm <doas::CmdValue> cmd
%nterm <std::optional<std::vector<std::string>>> args

%%

grammar:	/* empty */
		| grammar TNL
		| grammar rule TNL
		| error TNL
		;

rule:		action ident target cmd {
			doas::Rule rule;
			rule.action = $1.action;
			rule.options = $1.options;
			rule.envlist = std::move($1.envlist);
			rule.ident = std::move($2);
			rule.target = std::move($3);
			rule.cmd = std::move($4.cmd);
			rule.cmdargs = std::move($4.cmdargs);
			doas::g_rules.push_back(std::move(rule));
		} ;

action:		TPERMIT options {
			$$.action = doas::kPermit;
			$$.options = $2.options;
			$$.envlist = std::move($2.envlist);
		} | TDENY {
			$$.action = doas::kDeny;
			$$.options = 0;
			$$.envlist = std::nullopt;
		} ;

options:	/* none */ {
			$$.options = 0;
			$$.envlist = std::nullopt;
		} | options option {
			$$.options = $1.options | $2.options;
			$$.envlist = std::move($1.envlist);
			if (($$.options & (doas::kNopass | doas::kPersist)) ==
			    (doas::kNopass | doas::kPersist)) {
				config_error("can't combine nopass and persist");
				YYERROR;
			}
			if ($2.envlist) {
				if ($$.envlist) {
					config_error("can't have two setenv sections");
					YYERROR;
				} else {
					$$.envlist = std::move($2.envlist);
				}
			}
		} ;
option:		TNOPASS {
			$$.options = doas::kNopass;
			$$.envlist = std::nullopt;
		} | TNOLOG {
			$$.options = doas::kNolog;
			$$.envlist = std::nullopt;
		} | TPERSIST {
			$$.options = doas::kPersist;
			$$.envlist = std::nullopt;
		} | TKEEPENV {
			$$.options = doas::kKeepenv;
			$$.envlist = std::nullopt;
		} | TSETENV TLBRACE strlist TRBRACE {
			$$.options = 0;
			$$.envlist = std::move($3);
		} ;

strlist:	/* empty */ {
			$$ = std::vector<std::string> {};
		} | strlist TSTRING {
			$$ = std::move($1);
			$$.push_back(std::move($2));
		} ;


ident:		TSTRING {
			$$ = std::move($1);
		} ;

target:		/* optional */ {
			$$ = std::nullopt;
		} | TAS TSTRING {
			$$ = std::move($2);
		} ;

cmd:		/* optional */ {
			$$.cmd = std::nullopt;
			$$.cmdargs = std::nullopt;
		} | TCMD TSTRING args {
			$$.cmd = std::move($2);
			$$.cmdargs = std::move($3);
		} ;

args:		/* empty */ {
			$$ = std::nullopt;
		} | TARGS strlist {
			$$ = std::move($2);
		} ;

%%

namespace {

struct Keyword {
    const char* word;
    int token;
};

const Keyword kKeywords[] = {
    {"deny", doas::ConfigParser::token::TDENY},
    {"permit", doas::ConfigParser::token::TPERMIT},
    {"as", doas::ConfigParser::token::TAS},
    {"cmd", doas::ConfigParser::token::TCMD},
    {"args", doas::ConfigParser::token::TARGS},
    {"nopass", doas::ConfigParser::token::TNOPASS},
    {"nolog", doas::ConfigParser::token::TNOLOG},
    {"persist", doas::ConfigParser::token::TPERSIST},
    {"keepenv", doas::ConfigParser::token::TKEEPENV},
    {"setenv", doas::ConfigParser::token::TSETENV},
};

// yyerror() of the original: "doas: <msg> at line <n>" with n = yylval.lineno + 1.
// The "doas: " prefix is a literal in the original, not the program name.
void config_error(std::string_view msg) {
    sys::err_out(std::format("doas: {} at line {}\n", msg, g_lex.lineno + 1));
    doas::g_parse_errors++;
}

using Symbol = doas::ConfigParser::symbol_type;

Symbol make_token(int token) {
    switch (token) {
    case doas::ConfigParser::token::TDENY:
        return doas::ConfigParser::make_TDENY();
    case doas::ConfigParser::token::TPERMIT:
        return doas::ConfigParser::make_TPERMIT();
    case doas::ConfigParser::token::TAS:
        return doas::ConfigParser::make_TAS();
    case doas::ConfigParser::token::TCMD:
        return doas::ConfigParser::make_TCMD();
    case doas::ConfigParser::token::TARGS:
        return doas::ConfigParser::make_TARGS();
    case doas::ConfigParser::token::TNOPASS:
        return doas::ConfigParser::make_TNOPASS();
    case doas::ConfigParser::token::TNOLOG:
        return doas::ConfigParser::make_TNOLOG();
    case doas::ConfigParser::token::TPERSIST:
        return doas::ConfigParser::make_TPERSIST();
    case doas::ConfigParser::token::TKEEPENV:
        return doas::ConfigParser::make_TKEEPENV();
    case doas::ConfigParser::token::TSETENV:
        return doas::ConfigParser::make_TSETENV();
    default:
        return doas::ConfigParser::make_YYUNDEF();
    }
}

Symbol yylex() {
    std::array<char, 1024> buf {};
    char* p = buf.data();
    char* const ebuf = buf.data() + buf.size();
    int c = 0;
    int quotes = 0;
    int escape = 0;
    int qpos = -1;
    int nonkw = 0;

repeat:
    // Skip whitespace first.
    for (c = getc(g_lex.fp); c == ' ' || c == '\t'; c = getc(g_lex.fp)) {
        g_lex.colno++;
    }

    // Check for special one character constructions.
    switch (c) {
    case '\n':
        g_lex.colno = 0;
        g_lex.lineno++;
        return doas::ConfigParser::make_TNL();
    case '{':
        return doas::ConfigParser::make_TLBRACE();
    case '}':
        return doas::ConfigParser::make_TRBRACE();
    case '#':
        // Skip comments; NUL is allowed; no continuation.
        while ((c = getc(g_lex.fp)) != '\n') {
            if (c == EOF) {
                goto eof;
            }
        }
        g_lex.colno = 0;
        g_lex.lineno++;
        return doas::ConfigParser::make_TNL();
    case EOF:
        goto eof;
    default:
        break;
    }

    // Parsing the next word.
    for (;; c = getc(g_lex.fp), g_lex.colno++) {
        switch (c) {
        case '\0':
            config_error(std::format("unallowed character NUL in column {}", g_lex.colno + 1));
            escape = 0;
            continue;
        case '\\':
            escape = !escape;
            if (escape) {
                continue;
            }
            break;
        case '\n':
            if (quotes) {
                config_error(std::format("unterminated quotes in column {}", qpos + 1));
            }
            if (escape) {
                nonkw = 1;
                escape = 0;
                g_lex.colno = 0;
                g_lex.lineno++;
                continue;
            }
            goto eow;
        case EOF:
            if (escape) {
                config_error(std::format("unterminated escape in column {}", g_lex.colno));
            }
            if (quotes) {
                config_error(std::format("unterminated quotes in column {}", qpos + 1));
            }
            goto eow;
        case '{':
        case '}':
        case '#':
        case ' ':
        case '\t':
            if (!escape && !quotes) {
                goto eow;
            }
            break;
        case '"':
            if (!escape) {
                quotes = !quotes;
                if (quotes) {
                    nonkw = 1;
                    qpos = g_lex.colno;
                }
                continue;
            }
            break;
        default:
            break;
        }
        *p++ = static_cast<char>(c);
        if (p == ebuf) {
            config_error("too long line");
            p = buf.data();
        }
        escape = 0;
    }

eow:
    *p = 0;
    if (c != EOF) {
        ungetc(c, g_lex.fp);
    }
    if (p == buf.data()) {
        // There could be a number of reasons for an empty buffer, and we handle
        // all of them here, to avoid cluttering the main loop.
        if (c == EOF) {
            goto eof;
        }
        if (qpos == -1) { // accept, e.g., empty args: cmd foo args ""
            goto repeat;
        }
    }
    if (!nonkw) {
        for (const Keyword& keyword : kKeywords) {
            if (strcmp(buf.data(), keyword.word) == 0) {
                return make_token(keyword.token);
            }
        }
    }
    return doas::ConfigParser::make_TSTRING(std::string(buf.data()));

eof:
    if (ferror(g_lex.fp)) {
        config_error("input error reading config");
    }
    return doas::ConfigParser::make_YYEOF();
}

} // namespace

void doas::ConfigParser::error(const std::string& msg) {
    config_error(msg);
}

namespace doas {

int run_parser(std::FILE* fp) {
    g_lex.fp = fp;
    g_lex.lineno = 0;
    g_lex.colno = 0;
    ConfigParser parser;
    return parser.parse();
}

} // namespace doas
