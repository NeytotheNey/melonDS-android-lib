#!/bin/sh
# Build the four FreeBIOS images (NTR = DS, TWL = DSi; ARM9 and ARM7) from
# src/bios_common.s and regenerate src/FreeBIOS_Data.h, in the same layout
# as upstream melonDS (freebios/Makefile + Makefile.bios, xxd -i -c 16).
#
# Usage: ./build.sh [OUTDIR] [DATA_HEADER]
#   OUTDIR       where the .bin files go (default: ./build)
#   DATA_HEADER  generated header (default: ../src/FreeBIOS_Data.h;
#                pass "-" to skip)
#
# Toolchain: clang + ld.lld + llvm-objcopy by default. To use a GNU toolchain
# instead, set PREFIX (e.g. PREFIX=arm-none-eabi-) and it will use
# ${PREFIX}gcc / ${PREFIX}ld / ${PREFIX}objcopy; CC overrides the compiler
# driver. PYTHON overrides the Python used to write the header.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/build}
HDR=${2:-$HERE/../src/FreeBIOS_Data.h}
SRC=$HERE/src/bios_common.s
mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Windows Python does not understand MSYS paths
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

build() { # name platform target arch cpu maxsize
  name=$1; plat=$2; tgt=$3; arch=$4; cpu=$5; max=$6
  defs="-DPLATFORM_$plat -DTARGET_$tgt"
  if [ -n "${PREFIX:-}" ]; then
    ${CC:-${PREFIX}gcc} -x assembler-with-cpp $defs -march="$arch" -mcpu="$cpu" -c "$SRC" -o "$TMP/$name.o"
    ${PREFIX}ld -Ttext=0 --entry=0 -o "$TMP/$name.elf" "$TMP/$name.o"
    ${PREFIX}objcopy -O binary --only-section=.text "$TMP/$name.elf" "$OUT/$name.bin"
  else
    ${CC:-clang} --target=arm-none-eabi -x assembler-with-cpp $defs -march="$arch" -mcpu="$cpu" -c "$SRC" -o "$TMP/$name.o"
    ${LD:-ld.lld} -Ttext=0 --entry=0 -o "$TMP/$name.elf" "$TMP/$name.o"
    ${OBJCOPY:-llvm-objcopy} -O binary --only-section=.text "$TMP/$name.elf" "$OUT/$name.bin"
  fi
  # The images are not padded; FreeBIOSGet*() zero-fills up to the BIOS size
  actual=$(wc -c < "$OUT/$name.bin" | tr -d ' ')
  if [ "$actual" -eq 0 ] || [ "$actual" -gt "$max" ]; then
    echo "error: $name.bin is $actual bytes, must be 1-$max" >&2
    exit 1
  fi
  printf '%s.bin: %d of %d bytes\n' "$name" "$actual" "$max"
}

build bios_ntr_arm9 NTR ARM9 armv5te arm946e-s 4096
build bios_ntr_arm7 NTR ARM7 armv4t  arm7tdmi  16384
build bios_twl_arm9 TWL ARM9 armv5te arm946e-s 65536
build bios_twl_arm7 TWL ARM7 armv4t  arm7tdmi  65536

if [ "$HDR" != "-" ]; then
  if [ -z "${PYTHON:-}" ]; then
    if command -v python3 > /dev/null 2>&1; then PYTHON=python3; else PYTHON=python; fi
  fi
  "$PYTHON" -I - "$(native "$HDR")" "$(native "$OUT")" <<'PY'
import os, sys
hdr, out = sys.argv[1:3]
text = ''
for name in ('bios_ntr_arm9', 'bios_ntr_arm7', 'bios_twl_arm9', 'bios_twl_arm7'):
    data = open(os.path.join(out, name + '.bin'), 'rb').read()
    lines = ['  ' + ', '.join('0x%02x' % b for b in data[i:i+16])
             for i in range(0, len(data), 16)]
    text += 'unsigned char %s[] = {\n%s\n};\n' % (name, ',\n'.join(lines))
    text += 'unsigned int %s_len = %d;\n' % (name, len(data))
open(hdr, 'w', newline='\n').write(text)
PY
  echo "wrote $HDR"
fi
