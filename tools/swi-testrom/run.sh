#!/bin/sh
# Run the SWI test ROM in the swi-harness with the official BIOS and with
# FreeBIOS (DS mode), in every mode, then write the report.
#
# Usage: ./run.sh [OUTDIR]
#   OUTDIR  default: C:/Dev/melonDS-android/logs/swi/testrom (git-ignored)
#           runs go to OUTDIR/runs/<bios>/<mode>, the report to OUTDIR/report.txt
#
# Modes (poked into the control block at 02300000h before the first frame):
#   normal     every test that is safe to run in one go (mode 0)
#   iso-<id>   one isolated test (may hang, crash or jump to 0) per run (mode 1)
#   sr-<n>     SoftReset variant n (mode 2), see SOFTRESET in gen_tests.py
#
# Environment: HARNESS (swi-harness.exe), BIOSDIR (bios9.bin + bios7.bin,
# only handed to the emulator), BUILD (build.sh output dir), PYTHON,
# NORMAL_FRAMES / ISO_FRAMES / SR_FRAMES.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-/c/Dev/melonDS-android/logs/swi/testrom}
HARNESS=${HARNESS:-/c/Dev/swi-harness-data/build/swi-harness.exe}
BIOSDIR=${BIOSDIR:-C:/Dev/swi-harness-data/bios}
BUILD=${BUILD:-$HERE/build}
NORMAL_FRAMES=${NORMAL_FRAMES:-300}
ISO_FRAMES=${ISO_FRAMES:-120}
SR_FRAMES=${SR_FRAMES:-60}
if [ -z "${PYTHON:-}" ]; then
  if command -v python3 > /dev/null 2>&1; then PYTHON=python3; else PYTHON=python; fi
fi
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

ROM=$(native "$BUILD/swi-testrom.nds")
MANIFEST=$(native "$BUILD/tests.json")
[ -f "$BUILD/swi-testrom.nds" ] || { echo "no ROM in $BUILD, run build.sh first" >&2; exit 1; }

rm -rf "$OUT/runs"
mkdir -p "$OUT/runs"

run() { # bios name frames [harness options...]
  bios=$1; name=$2; frames=$3; shift 3
  dir=$OUT/runs/$bios/$name
  rm -rf "$dir"
  mkdir -p "$dir"
  if [ "$bios" = official ]; then set -- --bios official "$BIOSDIR" "$@"; else set -- --bios free "$@"; fi
  # clean room: no swi.jsonl/outputs.bin and no core.log (the core's unknown-I/O
  # messages can carry addresses the BIOS loaded)
  "$HARNESS" "$ROM" "$@" --frames "$frames" --out "$(native "$dir")" --shot-every 0 --no-swi-log --no-core-log \
    --dump-range 0x02200000 0x20000 > "$dir/run.log" 2>&1 || echo "harness exit $?" >> "$dir/run.log"
  echo "$frames" > "$dir/frames.txt"
  rm -f "$dir"/frame_*.bmp "$dir/frames.tsv"
}

ISO=$("$PYTHON" -I -c "import json,sys; print(' '.join(str(t['id']) for t in json.load(open(sys.argv[1]))['tests'] if t['isolated']))" "$MANIFEST")
SR=$("$PYTHON" -I -c "import json,sys; print(' '.join(str(v['variant']) for v in json.load(open(sys.argv[1]))['softreset']))" "$MANIFEST")

for bios in official free; do
  echo "$bios: normal"
  run "$bios" normal "$NORMAL_FRAMES"
  echo "$bios: $(echo "$ISO" | wc -w | tr -d ' ') isolated tests"
  for id in $ISO; do
    run "$bios" "iso-$id" "$ISO_FRAMES" --poke 0x02300000=1 --poke 0x02300004="$id"
  done
  echo "$bios: SoftReset variants $SR"
  for v in $SR; do
    run "$bios" "sr-$v" "$SR_FRAMES" --poke 0x02300000=2 --poke 0x02300004="$v"
  done
done

"$PYTHON" -I "$(native "$HERE/report.py")" "$MANIFEST" "$(native "$OUT/runs")" --out "$(native "$OUT/report.txt")"
