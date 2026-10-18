# doas-common

A single multicall binary that provides three applets through symlinks:

| Applet | Symlink | What it does |
|---|---|---|
| `doas` | `doas` | Execute a command as another user (port of [OpenDoas](https://github.com/Duncaen/OpenDoas), itself derived from OpenBSD `doas`) |
| `doasedit` | `doasedit` | Edit a file with elevated privileges using an unprivileged editor (port of [`doasedit`](https://github.com/nktmdzhg/doasedit)) |
| `sudo` | `sudo` | Translate a `sudo` command line into `doas` arguments and run it |

The three applets share one binary, so `doas` must be installed setuid root and
`doasedit`/`sudo` drop the effective privileges they inherit from it. `doasedit`
and `sudo` do not spawn a `doas` process: they call the doas applet in-process
(`doas::replace_main()`), and `doasedit` does so in a forked child so the doas
file operations keep working exactly like the original subprocess calls.

## Requirements

* Linux with glibc (PAM, `getspnam(3)`, `closefrom(3)` and `close_range(3)` are
  used directly; BSD/macOS are not supported).
* CMake ≥ 3.22, Ninja, Bison ≥ 3.8, an LLVM toolchain (clang, lld, llvm-ar,
  llvm-ranlib) and optionally ccache.
* libpam (default) or libcrypt, depending on the authentication provider.

## Building

```sh
cmake --preset default      # Ninja + clang + lld + ccache, Release
cmake --build build
ctest --preset default
```

`CMakePresets.json` is not tracked by git (see `.gitignore`); without it:

```sh
cmake -S . -B build -G Ninja \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

### Options

| Option | Default | Meaning |
|---|---|---|
| `DOAS_AUTH_PROVIDER` | `PAM` | `PAM` or `SHADOW`; replaces `configure --with-pam/--with-shadow` |
| `DOAS_PERSIST` | `OFF` | Build the timestamp/`persist` support (`configure --with-timestamp`) |
| `DOAS_PERSIST_DIR` | `/run/doas` | Directory holding timestamp files |
| `DOAS_UID_MAX` / `DOAS_GID_MAX` | `65535` | Upper bound accepted in `doas.conf` |
| `DOAS_SYSCONFDIR` | `/etc` | Directory holding `doas.conf` |
| `DOAS_INSTALL_SETUID` | `ON` | Install the binary with the setuid bit |
| `DOAS_SANITIZE` | *(empty)* | Sanitizer list for development builds |

The code is compiled with `-fno-exceptions -fno-rtti`; C++ runtime libraries are
linked statically (`-static-libstdc++ -static-libgcc`) while libc, libpam and
libcrypt stay dynamic, and `-ffunction-sections -fdata-sections` plus
`-Wl,--gc-sections` drop everything the binary does not reach.

## Installing

```sh
sudo cmake --install build          # /usr/bin/doas-common + doas/doasedit/sudo
```

The install rule sets mode `4755` (override with `-DDOAS_INSTALL_SETUID=OFF`) and
creates the three relative symlinks next to the binary. No PAM configuration file
is shipped: it is distribution specific and getting it wrong leaves the door
open, so `/etc/pam.d/doas` must be written and tested locally (the distribution's
`/etc/pam.d/sudo` is a reasonable starting point).

`doas.conf` lives in `/etc/doas.conf` (see `man 5 doas.conf`). It must be owned by
root and must not be writable by group or other; the applet refuses to start
otherwise.

## Usage

### doas

Behaviour, options, configuration grammar and diagnostics are unchanged with
respect to OpenDoas: `-C config`, `-L`, `-n`, `-s`, `-u user`, the same
`doas.conf` grammar and the same messages. See `man 1 doas` and `man 5
doas.conf`.

### doasedit

```sh
doasedit /etc/hosts /etc/fstab
DOAS_EDITOR=nano doasedit /etc/doas.conf
```

The editor is taken from `DOAS_EDITOR`, `VISUAL`, `EDITOR` or `vi`. Files owned
by the invoking user, files in directories owned by them and directories that
are user-writable are refused, and `doas.conf` edits are validated with `doas
-C` before they are written back. The privileged steps (`doas cat`, `doas stat`,
`doas dd`) are subject to the same `doas.conf` rules as before; see `man 1
doasedit`.

### sudo

`sudo` translates the command line and then runs the doas applet in-process:

```
$ sudo whoami
sudo <<< sudo whoami
sudo >>> /usr/bin/doas whoami
root
```

Flags that cannot be translated are ignored and left to `doas.conf`; the full
table is in `man 1 sudo`. `SUDO_USER`, `SUDO_UID` and `SUDO_GID` are exported,
and `SUDO_HIDE` suppresses the trace lines.

## Tests

`ctest` runs unit tests plus a golden corpus: every configuration in
`tests/fixtures/configs` is parsed by the applet and its stdout, stderr and exit
status are compared byte for byte with the output of the reference C
implementation (`tests/fixtures/golden`). Regenerate the expectations with:

```sh
tests/tools/regen_golden.sh /path/to/reference/doas
```

where the reference binary is built from the commit before the C++ port:

```sh
git worktree add /tmp/doas-ref 6266763
(cd /tmp/doas-ref && ./configure && make)
```

`cmake --build build --target tidy` runs clang-tidy and `--target format`
applies `.clang-format` (four space indentation, 100 columns).

## Third-party code

* `src/doas/*`, `src/sys/*`: ISC/BSD licensed, from OpenDoas/OpenBSD; copyright
  notices are retained in the files.
* `src/sudo/*`: BSD-2-Clause, copyright 2023 Paco Pascal; notice retained in the
  files.
* `src/doasedit/*`: MIT, copyright 2025 Nguyen Hoang Ky; notice retained in the
  files.
