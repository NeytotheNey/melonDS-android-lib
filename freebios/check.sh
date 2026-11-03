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

# The hang table in UPGRADE_PLAN.md, rows
#   | image | accurate address | experimental address | `label` | ...
# with "-" for a hang the variant does not have.
tr -d '\r' < "$HERE/UPGRADE_PLAN.md" | awk -F'|' '/^\| bios_(ntr|twl)_arm[79] /{
  for (i = 2; i <= 5; i++) gsub(/[ `]/, "", $i)
  if ($3 != "-") print $2, toupper($3), $5 > "/dev/stderr"
  if ($4 != "-") print $2, toupper($4), $5 }' 2> "$TMP/table-acc.txt" | sort > "$TMP/table-exp.txt"
sort -o "$TMP/table-acc.txt" "$TMP/table-acc.txt"
for v in acc exp; do
  if [ $v = acc ]; then built=$TMP/hangs.txt; else built=$TMP/experimental/hangs.txt; fi
  sort "$built" > "$TMP/built-$v.txt"
  if ! cmp -s "$TMP/table-$v.txt" "$TMP/built-$v.txt"; then
    echo "MISMATCH: the hang table in UPGRADE_PLAN.md differs from the $v build" >&2
    echo "(< table, > build):" >&2
    diff "$TMP/table-$v.txt" "$TMP/built-$v.txt" >&2 || true
    exit 1
  fi
done
echo "OK: the hang table in UPGRADE_PLAN.md matches the build"
