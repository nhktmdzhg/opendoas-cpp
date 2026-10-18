#!/bin/sh
# Regenerates tests/fixtures/golden from a reference (C) doas binary.
#
#   tests/tools/regen_golden.sh /path/to/reference/doas
#
# The reference binary can be built from the commit before the C++ port:
#
#   git worktree add /tmp/doas-ref <commit>
#   (cd /tmp/doas-ref && ./configure && make)
#
# Every fixture is run with `doas -C <config> [args...]`, which parses the
# configuration and, when arguments are given, reports permit/deny. The
# configuration paths are relative, so run this script from anywhere: it always
# changes into tests/fixtures first.
set -eu

ref=${1:?usage: regen_golden.sh /path/to/reference/doas}
here=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$here/fixtures"

golden=golden
mkdir -p "$golden"

run() {
    name=$1
    shift
    set +e
    "$ref" "$@" >"$golden/$name.out" 2>"$golden/$name.err"
    status=$?
    set -e
    printf '%s\n' "$status" >"$golden/$name.code"
}

for conf in configs/*.conf; do
    base=$(basename "$conf" .conf)
    run "$base" -C "$conf"

    if [ -f "cases/$base.argv" ]; then
        set --
        while IFS= read -r line; do
            set -- "$@" "$line"
        done <"cases/$base.argv"
        run "$base.args" -C "$conf" "$@"
    fi
done

# Synthetic cases, mirrored by tests/t_golden.cpp: inputs that cannot live in a
# text fixture.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

{
    printf 'permit '
    i=0
    while [ $i -lt 1030 ]; do
        printf 'a'
        i=$((i + 1))
    done
    printf '\n'
} >"$tmp/longline.conf"
run longline -C "$tmp/longline.conf"

printf 'permit nouser\000\n' >"$tmp/nul.conf"
run nul -C "$tmp/nul.conf"

run _missing -C configs/does-not-exist.conf

echo "golden files written to $here/fixtures/$golden"
