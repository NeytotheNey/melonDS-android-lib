#!/bin/sh
# Build the FreeBIOS ARM7/ARM9 images and regenerate the arrays in
# src/FreeBIOS.cpp.
#
# Usage: ./build.sh [OUTDIR] [CPP_FILE]
#   OUTDIR    where the .bin files go (default: this directory)
#   CPP_FILE  C++ file whose arrays are regenerated (default: ../src/FreeBIOS.cpp;
#             pass "-" to skip)
#
# Toolchain: clang + ld.lld + llvm-objcopy by default. To use a GNU toolchain
# instead, set PREFIX (e.g. PREFIX=arm-none-eabi-) and it will use
# ${PREFIX}gcc / ${PREFIX}objcopy; CC overrides the compiler driver.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE}
CPP=${2:-$HERE/../src/FreeBIOS.cpp}
SRC=$HERE/bios_common.S
mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

build() { # name define arch cpu size
  name=$1; def=$2; arch=$3; cpu=$4; size=$5
  if [ -n "${PREFIX:-}" ]; then
    ${CC:-${PREFIX}gcc} -D"$def" -march="$arch" -mcpu="$cpu" -c "$SRC" -o "$TMP/$name.o"
    ${PREFIX}ld -Ttext=0 --entry=0 -o "$TMP/$name.elf" "$TMP/$name.o"
    ${PREFIX}objcopy -O binary --only-section=.text "$TMP/$name.elf" "$OUT/$name.bin"
  else
    ${CC:-clang} --target=arm-none-eabi -march="$arch" -mcpu="$cpu" -D"$def" -c "$SRC" -o "$TMP/$name.o"
    ${LD:-ld.lld} -Ttext=0 --entry=0 -o "$TMP/$name.elf" "$TMP/$name.o"
    ${OBJCOPY:-llvm-objcopy} -O binary --only-section=.text "$TMP/$name.elf" "$OUT/$name.bin"
  fi
  actual=$(wc -c < "$OUT/$name.bin" | tr -d ' ')
  if [ "$actual" != "$size" ]; then
    echo "error: $name.bin is $actual bytes, expected $size" >&2
    exit 1
  fi
}

build drastic_bios_arm7 BIOS_ARM7 armv4t  arm7tdmi 16384
build drastic_bios_arm9 BIOS_ARM9 armv5te arm946e-s 4096

if [ "$CPP" != "-" ]; then
  python3 -I - "$CPP" "$OUT/drastic_bios_arm7.bin" "$OUT/drastic_bios_arm9.bin" <<'PY'
import re, sys
cpp, a7, a9 = sys.argv[1:4]
def body(path):
    data = open(path, 'rb').read()
    lines = []
    for i in range(0, len(data), 12):
        lines.append('  ' + ', '.join('0x%02x' % b for b in data[i:i+12]))
    return ',\n'.join(lines) + '\n'
text = open(cpp, newline='').read()
for name, path in (('bios_arm7_bin', a7), ('bios_arm9_bin', a9)):
    pat = re.compile(r'(std::array<u8, \w+> ' + name + r' = \{\n)(.*?)(\};)', re.S)
    text, n = pat.subn(lambda m: m.group(1) + body(path) + m.group(3), text, count=1)
    if n != 1:
        sys.exit('error: array %s not found in %s' % (name, cpp))
open(cpp, 'w', newline='').write(text)
PY
fi
echo "built $OUT/drastic_bios_arm7.bin $OUT/drastic_bios_arm9.bin"
