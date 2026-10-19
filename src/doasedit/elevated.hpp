#ifndef DOAS_DOASEDEDIT_ELEVATED_HPP
#define DOAS_DOASEDEDIT_ELEVATED_HPP

#include "sys/error.hpp"
#include "sys/fs.hpp"

#include <string>
#include <string_view>
#include <sys/stat.h>

namespace doasedit::elevated {

// Result of an operation that streams data back through a pipe.
struct Text {
    int status = 1;
    std::string out;
};

// File operations that need root, run natively instead of spawning
// "doas cat"/"doas dd"/"doas stat": a child raises its effective uid back to 0 from
// the saved set user id, runs doas's rule matching and authentication for the same
// command line the original passed to doas, performs the syscall and exits.
//
// The first successful authorize_command() of the process answers the password
// prompt; every later operation of the same run still goes through permit() but is
// not asked again.
//
// Pre-flight checks, used to fail before the editor opens. authorize_write() retries
// `attempts` times. Both return 0 on success and the child's exit status otherwise.
[[nodiscard]] int authorize_write(std::string_view tmp_file, std::string_view target, int attempts);
[[nodiscard]] int authorize_read(std::string_view path);

// stat(2) as root; the error is the errno the child saw, or ENOENT when the child
// could not even run.
[[nodiscard]] sys::Result<struct stat> stat_path(std::string_view path);

// Read the whole file as root; out holds the content when status is 0, otherwise
// status is the errno.
[[nodiscard]] Text read_path(std::string_view path);

// Write the content of `source` (opened by the parent, the temp file the editor
// worked on) over `path` as root: O_WRONLY|O_CREAT|O_TRUNC with mode 0666, exactly
// like the "dd if=... of=..." it replaces, so the inode, owner and mode of the
// target stay untouched. `tmp_file` is only used to build that same command line for
// doas's rule matching. 0 on success, errno otherwise.
[[nodiscard]] int
write_path(std::string_view path, std::string_view tmp_file, const sys::fs::Fd& source);

} // namespace doasedit::elevated

#endif // DOAS_DOASEDEDIT_ELEVATED_HPP
