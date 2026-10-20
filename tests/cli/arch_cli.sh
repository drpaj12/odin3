#!/usr/bin/env bash
# arch_cli.sh ODIN3 ARCH_XML: read_arch then write_techlib to stdout through the CLI.
set -euo pipefail
[[ $# -eq 2 ]] || { echo "usage: arch_cli.sh ODIN3 ARCH_XML" >&2; exit 2; }
odin3=$1
xml=$2
out=$("$odin3" -p "read_arch $xml; write_techlib -")
grep -q '^library ' <<<"$out" || { echo "no library line" >&2; exit 1; }
grep -q '^cell multiply hard$' <<<"$out" || { echo "no multiply cell" >&2; exit 1; }
if "$odin3" -p "write_techlib -" >/dev/null 2>&1; then
    echo "write_techlib without an architecture must fail" >&2
    exit 1
fi
if "$odin3" -p "read_arch $xml.nope" >/dev/null 2>&1; then
    echo "read_arch of a missing file must fail" >&2
    exit 1
fi
