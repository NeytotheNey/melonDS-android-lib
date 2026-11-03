#!/bin/sh
# Build the four FreeBIOS images (NTR = DS, TWL = DSi; ARM9 and ARM7) from
# src/bios_common.s and regenerate src/FreeBIOS_Data.h, in the same layout
# as upstream melonDS (freebios/Makefile + Makefile.bios, xxd -i -c 16).
#
# Two variants come from the same source:
#   accurate (default)  matches the official BIOS, deliberate hangs included.
#                       OUTDIR/bios_*.bin, embedded in src/FreeBIOS_Data.h.
#   experimental        -DFREEBIOS_EXPERIMENTAL: the old "safe" choices (x/0
#                       returns, absent SWIs return, DSi7 IntrWait on IF bits
#                       returns, DTCM-relative SoftReset stacks). Not
#                       embedded; written to OUTDIR/experimental/ as files the
#                       app loads from a custom BIOS folder:
#                         bios9.bin (4096) and bios7.bin (16384)      NTR, DS folder
#                         dsi/bios9.bin and dsi/bios7.bin (65536)     TWL, DSi folder
#                       plus the unpadded bios_*.bin and hangs.txt.
#
# Usage: ./build.sh [OUTDIR] [DATA_HEADER]
#   OUTDIR       where the .bin files go (default: ./build)
#   DATA_HEADER  generated header (default: ../src/FreeBIOS_Data.h;
#                pass "-" to skip)
#
# Toolchain: clang + ld.lld + llvm-objcopy by default. To use a GNU toolchain
# instead, set PREFIX (e.g. PREFIX=arm-none-eabi-) and it will use
# ${PREFIX}gcc / ${PREFIX}ld / ${PREFIX}objcopy; CC overrides the compiler
# driver. PYTHON overrides the Python used to write the header. NM (default
# llvm-nm, or ${PREFIX}nm) reads the hang labels.
#
# It also prints the address of every deliberate hang (hang_* labels, plus
# halt_arm9_cp15) per image and writes them to OUTDIR/hangs.txt as
# "image address label" (OUTDIR/experimental/hangs.txt for the experimental
# variant); check.sh compares both lists with the "Where FreeBIOS hangs on
# purpose" table in UPGRADE_PLAN.md.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/build}
HDR=${2:-$HERE/../src/FreeBIOS_Data.h}
SRC=$HERE/src/bios_common.s
mkdir -p "$OUT/experimental/dsi"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Windows Python does not understand MSYS paths
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

# Variant settings used by build(): VOUT (output dir) and VDEFS (extra defines)
build() { # name platform target arch cpu maxsize base
  name=$1; plat=$2; tgt=$3; arch=$4; cpu=$5; max=$6; base=$7
  defs="-DPLATFORM_$plat -DTARGET_$tgt $VDEFS"
  if [ -n "${PREFIX:-}" ]; then
    ${CC:-${PREFIX}gcc} -x assembler-with-cpp $defs -march="$arch" -mcpu="$cpu" -c "$SRC" -o "$TMP/$name.o"
    ${PREFIX}ld -Ttext=0 --entry=0 -o "$TMP/$name.elf" "$TMP/$name.o"
    ${PREFIX}objcopy -O binary --only-section=.text "$TMP/$name.elf" "$VOUT/$name.bin"
    nm=${NM:-${PREFIX}nm}
  else
    ${CC:-clang} --target=arm-none-eabi -x assembler-with-cpp $defs -march="$arch" -mcpu="$cpu" -c "$SRC" -o "$TMP/$name.o"
    ${LD:-ld.lld} -Ttext=0 --entry=0 -o "$TMP/$name.elf" "$TMP/$name.o"
    ${OBJCOPY:-llvm-objcopy} -O binary --only-section=.text "$TMP/$name.elf" "$VOUT/$name.bin"
    nm=${NM:-llvm-nm}
  fi
  # The images are not padded; FreeBIOSGet*() zero-fills up to the BIOS size
  actual=$(wc -c < "$VOUT/$name.bin" | tr -d ' ')
  if [ "$actual" -eq 0 ] || [ "$actual" -gt "$max" ]; then
    echo "error: $name.bin is $actual bytes, must be 1-$max" >&2
    exit 1
  fi
  printf '%s%s.bin: %d of %d bytes\n' "$VNAME" "$name" "$actual" "$max"
  # Deliberate hangs, at the address the CPU sees (ARM9 BIOS at FFFF0000h)
  $nm "$TMP/$name.elf" | while read -r addr type sym; do
    case $sym in
      hang_*|halt_arm9_cp15)
        line=$(printf '%s %08X %s' "$name" $((0x$addr + base)) "$sym")
        echo "  $line"
        echo "$line" >> "$VOUT/hangs.txt";;
    esac
  done
}

build_all() {
  : > "$VOUT/hangs.txt"
  build bios_ntr_arm9 NTR ARM9 armv5te arm946e-s 4096  0xFFFF0000
  build bios_ntr_arm7 NTR ARM7 armv4t  arm7tdmi  16384 0
  build bios_twl_arm9 TWL ARM9 armv5te arm946e-s 65536 0xFFFF0000
  build bios_twl_arm7 TWL ARM7 armv4t  arm7tdmi  65536 0
}

# zero-pad $1 to $3 bytes into $2 (the app checks the exact file size)
pad() { cat "$1" /dev/zero | head -c "$3" > "$2"; }

VOUT=$OUT; VDEFS=; VNAME=
build_all
VOUT=$OUT/experimental; VDEFS=-DFREEBIOS_EXPERIMENTAL; VNAME=experimental/
build_all
pad "$VOUT/bios_ntr_arm9.bin" "$VOUT/bios9.bin" 4096
pad "$VOUT/bios_ntr_arm7.bin" "$VOUT/bios7.bin" 16384
pad "$VOUT/bios_twl_arm9.bin" "$VOUT/dsi/bios9.bin" 65536
pad "$VOUT/bios_twl_arm7.bin" "$VOUT/dsi/bios7.bin" 65536
echo "wrote $VOUT/bios9.bin, bios7.bin, dsi/bios9.bin, dsi/bios7.bin"

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
