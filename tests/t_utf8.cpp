// UTF-8 validation used by doasedit (Rust's str::to_str / env::var reject
// invalid UTF-8).
#include "sys/strings.hpp"
#include "test_util.hpp"

#include <string>

int main() {
    CHECK(sys::utf8_valid(""));
    CHECK(sys::utf8_valid("hello"));
    CHECK(sys::utf8_valid("/etc/hosts"));
    CHECK(sys::utf8_valid("caf\xc3\xa9"));      // U+00E9
    CHECK(sys::utf8_valid("\xe2\x82\xac"));     // U+20AC
    CHECK(sys::utf8_valid("\xf0\x9f\x98\x80")); // U+1F600
    CHECK(sys::utf8_valid("\x7f"));

    CHECK(!sys::utf8_valid("\x80")); // stray continuation
    CHECK(!sys::utf8_valid("\xbf"));
    CHECK(!sys::utf8_valid("\xc0\x80"));         // overlong NUL
    CHECK(!sys::utf8_valid("\xc1\xbf"));         // overlong
    CHECK(!sys::utf8_valid("\xc2"));             // truncated
    CHECK(!sys::utf8_valid("\xe2\x82"));         // truncated
    CHECK(!sys::utf8_valid("\xf0\x9f\x98"));     // truncated
    CHECK(!sys::utf8_valid("\xed\xa0\x80"));     // surrogate D800
    CHECK(!sys::utf8_valid("\xed\xbf\xbf"));     // surrogate DFFF
    CHECK(!sys::utf8_valid("\xf4\x90\x80\x80")); // > U+10FFFF
    CHECK(!sys::utf8_valid("\xf5\x80\x80\x80")); // invalid lead
    CHECK(!sys::utf8_valid("\xff"));
    CHECK(!sys::utf8_valid("ok\xc3"));   // truncated tail
    CHECK(!sys::utf8_valid("\xc3\x28")); // bad continuation

    return test::finish("t_utf8");
}
