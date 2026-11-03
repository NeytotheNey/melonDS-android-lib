#!/bin/sh
# Build the SWI test ROM (tools/swi-testrom) from sources: generate the test
# table and data (gen_tests.py), assemble/compile the ARM9 and ARM7 programs
# with clang + ld.lld + llvm-objcopy, and pack them into an .nds (mkrom.py).
#
# Usage: ./build.sh [OUTDIR]
#   OUTDIR  where everything goes (default: ./build, git-ignored)
#           result: OUTDIR/swi-testrom.nds and OUTDIR/tests.json (DS mode),
#           OUTDIR/twl/swi-testrom-twl.nds and OUTDIR/twl/tests.json (DSi
#           mode: TWL header, built with -DTWL)
#
# Toolchain: the NDK's LLVM if NDK_LLVM (default
# C:/Tools/android-sdk/ndk/28.0.13004108/toolchains/llvm/prebuilt/windows-x86_64/bin)
# exists, otherwise clang/ld.lld/llvm-objcopy from PATH. CC, LD, OBJCOPY and
# PYTHON override single tools.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/build}
mkdir -p "$OUT"

NDK_LLVM=${NDK_LLVM:-/c/Tools/android-sdk/ndk/28.0.13004108/toolchains/llvm/prebuilt/windows-x86_64/bin}
if [ -x "$NDK_LLVM/clang.exe" ] || [ -x "$NDK_LLVM/clang" ]; then
  CC=${CC:-$NDK_LLVM/clang}
  LD=${LD:-$NDK_LLVM/ld.lld}
  OBJCOPY=${OBJCOPY:-$NDK_LLVM/llvm-objcopy}
fi
CC=${CC:-clang}
LD=${LD:-ld.lld}
OBJCOPY=${OBJCOPY:-llvm-objcopy}
if [ -z "${PYTHON:-}" ]; then
  if command -v python3 > /dev/null 2>&1; then PYTHON=python3; else PYTHON=python; fi
fi

# Windows Python does not understand MSYS paths
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

CFLAGS0="--target=arm-none-eabi -marm -O2 -ffreestanding -nostdlib -fno-builtin -fno-pic
  -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -mfloat-abi=soft
  -Wall -Werror -I$HERE/src"

build() { # cpu arch mcpu loadaddr (uses OUT and CFLAGS)
  cpu=$1; arch=$2; mcpu=$3; addr=$4
  # shellcheck disable=SC2086
  "$CC" $CFLAGS -DARM$cpu -march="$arch" -mcpu="$mcpu" -x assembler-with-cpp -c "$HERE/src/crt.S" -o "$OUT/crt$cpu.o"
  # shellcheck disable=SC2086
  "$CC" $CFLAGS -DARM$cpu -march="$arch" -mcpu="$mcpu" -c "$HERE/src/runner.c" -o "$OUT/runner$cpu.o"
  "$LD" -T "$HERE/src/rom.ld" --defsym=LOAD_ADDR="$addr" -o "$OUT/arm$cpu.elf" "$OUT/crt$cpu.o" "$OUT/runner$cpu.o"
  "$OBJCOPY" -O binary "$OUT/arm$cpu.elf" "$OUT/arm$cpu.bin"
  printf 'arm%s.bin: %d bytes at %s\n' "$cpu" "$(wc -c < "$OUT/arm$cpu.bin" | tr -d ' ')" "$addr"
}

# DS mode
"$PYTHON" -I "$(native "$HERE/gen_tests.py")" "$(native "$OUT")"
CFLAGS="$CFLAGS0 -I$OUT"
build 9 armv5te arm946e-s 0x02000000
build 7 armv4t arm7tdmi 0x02380000
"$PYTHON" -I "$(native "$HERE/mkrom.py")" "$(native "$OUT/arm9.bin")" "$(native "$OUT/arm7.bin")" "$(native "$OUT/swi-testrom.nds")"

# DSi mode
DS_OUT=$OUT
OUT=$DS_OUT/twl
mkdir -p "$OUT"
"$PYTHON" -I "$(native "$HERE/gen_tests.py")" --twl "$(native "$OUT")"
CFLAGS="$CFLAGS0 -DTWL -I$OUT"
build 9 armv5te arm946e-s 0x02000000
build 7 armv4t arm7tdmi 0x02380000
"$PYTHON" -I "$(native "$HERE/mkrom.py")" --twl "$(native "$OUT/arm9.bin")" "$(native "$OUT/arm7.bin")" "$(native "$OUT/swi-testrom-twl.nds")"
