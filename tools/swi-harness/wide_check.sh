#!/bin/sh
# The headless WideMelon check (docs/plan-widemelon.md step 2, see WideCheck.cpp): builds the
# harness and runs each game below with WideMelon off and on, failing if the middle 256 or
# anything the game can see differs. From the core repo, in Git Bash:
#
#   sh tools/swi-harness/wide_check.sh
#
# Needs MSYS2 MinGW (gcc/g++ in /c/msys64/mingw64/bin), CMake and Ninja. Override with GAMES (the
# ROM folder, only read), OUT (results, default the app repo's git-ignored logs/wide-check), BUILD
# and WIDTH (default 342, 16:9). Savestates are copied into $OUT/states first and only read there.

GAMES=${GAMES:-/c/Users/neyto/OneDrive/Documents/Games}
OUT=${OUT:-/c/Dev/melonDS-android/logs/wide-check}
BUILD=${BUILD:-/c/Dev/swi-harness-data/build-widecheck}
WIDTH=${WIDTH:-342}
HERE=$(cd "$(dirname "$0")" && pwd)
export PATH=/c/msys64/mingw64/bin:$PATH

mkdir -p "$OUT/states" || exit 2
if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -G Ninja -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ > "$OUT/build.log" 2>&1 || { echo "configure failed, see $OUT/build.log"; exit 2; }
fi
cmake --build "$BUILD" >> "$OUT/build.log" 2>&1 || { echo "build failed, see $OUT/build.log"; exit 2; }

MKDS="$GAMES/Mario Kart DS/Mario Kart DS (USA) (En,Fr,De,Es,It)"
PH="$GAMES/Legend of Zelda, The - Phantom Hourglass (USA)/Legend of Zelda, The - Phantom Hourglass (USA) (En,Fr,Es)"
W2="$GAMES/Pokemon_ White Version 2/Pokemon - White Version 2 (USA, Europe) (NDSi Enhanced)"
cp "$MKDS.ml1" "$OUT/states/mkds.ml1" && cp "$W2.ml3" "$OUT/states/w2.ml3" || exit 2

failed=0
run() # name rom.nds frames [options]
{
    name=$1; rom=$2; frames=$3; shift 3
    "$BUILD/swi-harness.exe" "$rom" --wide-check "$WIDTH" --frames "$frames" --out "$OUT/$name" "$@" 2>/dev/null
    case $? in 0) ;; 1) failed=1 ;; *) echo "$name: couldn't run"; failed=1 ;; esac
}

# boot, menus, then the attract demo race from about frame 1420 (top 3D, bottom map)
run mkds-demo "$MKDS.nds" 2400
# a savestate after a race (3D results screen)
run mkds-state "$MKDS.nds" 600 --state "$OUT/states/mkds.ml1"
# title and intro, 3D from about frame 230; reads the polygon RAM counts every frame
run ph-intro "$PH.nds" 1200
# overworld from a savestate; culls with box tests
run w2-state "$W2.nds" 600 --state "$OUT/states/w2.ml3"
# the same with box tests squeezed as WideMelon does them, so memory differs and is only reported
run w2-state-boxwide "$W2.nds" 600 --state "$OUT/states/w2.ml3" --box-test wide

[ $failed = 0 ] && echo "wide check: all passed" || echo "wide check: FAILED (diffs.txt in each $OUT/<game>)"
exit $failed
