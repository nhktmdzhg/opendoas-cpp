#ifndef DOAS_DOAS_DOAS_HPP
#define DOAS_DOAS_DOAS_HPP

namespace doas {

// The doas applet: full port of doas.c main(). Never returns: it execs the target
// command, exits through an error path, or exits from the -C/-L handling.
[[noreturn]] void replace_main(int argc, char* const argv[]);

// "usage: doas [-Lns] [-C config] [-u user] command [args]" on stderr, exit 1.
[[noreturn]] void usage();

} // namespace doas

#endif // DOAS_DOAS_DOAS_HPP
