#!/bin/sh
# Rebuild the FreeBIOS into a temp dir and verify that src/FreeBIOS_Data.h
# matches src/bios_common.s byte for byte (line endings are ignored, since a
# Windows checkout may have CRLF).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
HDR=$HERE/../src/FreeBIOS_Data.h
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$HERE/build.sh" "$TMP" "$TMP/FreeBIOS_Data.h" > /dev/null
if ! tr -d '\r' < "$HDR" | cmp -s - "$TMP/FreeBIOS_Data.h"; then
  echo "MISMATCH: src/FreeBIOS_Data.h differs from freebios/src/bios_common.s" >&2
  exit 1
fi
echo "OK: src/FreeBIOS_Data.h matches freebios/src/bios_common.s"
