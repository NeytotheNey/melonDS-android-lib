#!/bin/sh
# Rebuild the FreeBIOS into a temp dir and verify that the committed .bin files
# and the arrays in src/FreeBIOS.cpp match the assembly source byte for byte.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CPP=$HERE/../src/FreeBIOS.cpp
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$CPP" "$TMP/FreeBIOS.cpp"
"$HERE/build.sh" "$TMP" "$TMP/FreeBIOS.cpp" > /dev/null
fail=0
for n in drastic_bios_arm7 drastic_bios_arm9; do
  if ! cmp -s "$TMP/$n.bin" "$HERE/$n.bin"; then
    echo "MISMATCH: freebios/$n.bin differs from the assembly source" >&2; fail=1
  fi
done
if ! cmp -s "$TMP/FreeBIOS.cpp" "$CPP"; then
  echo "MISMATCH: src/FreeBIOS.cpp arrays differ from the assembly source" >&2; fail=1
fi
[ $fail -eq 0 ] && echo "OK: .bin files and src/FreeBIOS.cpp match bios_common.S"
exit $fail
