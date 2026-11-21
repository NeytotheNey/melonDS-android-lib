#!/bin/sh
# Run the SWI test ROM in the swi-harness with the official BIOS and with
# FreeBIOS, in every mode, then write the reports: DS mode (official NDS BIOS
# vs NTR FreeBIOS) and DSi mode (official DSi BIOS vs TWL FreeBIOS).
#
# Usage: ./run.sh [OUTDIR]
#   OUTDIR  default: D:/Dev/melonadeDS/logs/swi/testrom (git-ignored)
#           DS:  runs go to OUTDIR/runs/<bios>/<mode>, the report to OUTDIR/report.txt
#           DSi: runs go to OUTDIR/runs-dsi/<bios>/<mode>, the report to OUTDIR/report-dsi.txt
#
# Modes (poked into the control block at 02300000h before the first frame):
#   normal     every test that is safe to run in one go (mode 0)
#   iso-<id>   one isolated test (may hang, crash or jump to 0) per run (mode 1)
#   sr-<n>     SoftReset variant n (mode 2, DS only), see SOFTRESET in gen_tests.py
#
# DSi mode boots build.sh's TWL ROM (OUTDIR of build.sh/twl) with --console dsi:
# official = the DSi BIOS dumps in DSIDIR (and the NDS dumps in BIOSDIR),
# free = our TWL FreeBIOS (--dsi-bios free) and NTR FreeBIOS. The DSi runs share
# one copy of DSIDIR/nand.bin in OUTDIR (nand-run.bin), opened read-only by the
# harness (--nand-readonly: no touch calibration patch, which the ROM doesn't
# need), so no run can change it; the copy is checked against the original at
# the end and deleted on exit, also on Ctrl-C. The BIOS, NAND and firmware
# files are only handed to the emulator.
#
# Environment: HARNESS (swi-harness.exe), BIOSDIR (bios9.bin + bios7.bin),
# DSIDIR (DSi bios9.bin, bios7.bin, firmware.bin, nand.bin), BUILD (build.sh
# output dir), PYTHON, MODES ("ds dsi" by default; "ds" or "dsi" for one),
# NORMAL_FRAMES / ISO_FRAMES / SR_FRAMES / DSI_NORMAL_FRAMES,
# BIOSES ("official free" by default; add free-exp for the experimental
# FreeBIOS files in EXPDIR, default freebios/build/experimental: bios9.bin and
# bios7.bin for DS mode, dsi/ for DSi mode). Each FreeBIOS variant gets its
# own report against official: report.txt / report-dsi.txt for free,
# report-<variant>.txt / report-dsi-<variant>.txt for the others. Only the
# runs of the BIOSes in BIOSES are redone; without official in BIOSES the
# earlier official runs are reused (they must exist and come from the same
# ROM build).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-/d/Dev/melonadeDS/logs/swi/testrom}
HARNESS=${HARNESS:-/d/Dev/swi-harness-data/build/swi-harness.exe}
BIOSDIR=${BIOSDIR:-D:/Dev/swi-harness-data/bios}
DSIDIR=${DSIDIR:-D:/Dev/swi-harness-data/dsi}
BUILD=${BUILD:-$HERE/build}
MODES=${MODES:-ds dsi}
BIOSES=${BIOSES:-official free}
EXPDIR=${EXPDIR:-$HERE/../../freebios/build/experimental}
NORMAL_FRAMES=${NORMAL_FRAMES:-300}
DSI_NORMAL_FRAMES=${DSI_NORMAL_FRAMES:-600}
ISO_FRAMES=${ISO_FRAMES:-120}
SR_FRAMES=${SR_FRAMES:-60}
if [ -z "${PYTHON:-}" ]; then
  if command -v python3 > /dev/null 2>&1; then PYTHON=python3; else PYTHON=python; fi
fi
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

mkdir -p "$OUT"
NAND=$OUT/nand-run.bin
cleanup() { rm -f "$NAND"; }
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

run() { # console bios name frames [harness options...]
  console=$1; bios=$2; name=$3; frames=$4; shift 4
  if [ "$console" = dsi ]; then dir=$OUT/runs-dsi/$bios/$name; else dir=$OUT/runs/$bios/$name; fi
  rm -rf "$dir"
  mkdir -p "$dir"
  case $bios in
    official) set -- --bios official "$BIOSDIR" "$@"; dsibios=official; dsibiosdir= ;;
    free) set -- --bios free "$@"; dsibios=free; dsibiosdir= ;;
    free-exp) set -- --bios file "$(native "$EXPDIR")" "$@"; dsibios=file; dsibiosdir=$(native "$EXPDIR/dsi") ;;
    *) echo "unknown BIOS $bios in BIOSES" >&2; exit 1 ;;
  esac
  if [ "$console" = dsi ]; then
    set -- "$@" --console dsi --dsi "$DSIDIR" --dsi-bios "$dsibios" ${dsibiosdir:+"$dsibiosdir"} \
      --nand "$(native "$NAND")" --nand-readonly
    rom=$ROM_TWL; range=0x40000
  else
    rom=$ROM; range=0x20000
  fi
  # clean room: no swi.jsonl/outputs.bin and no core.log (the core's unknown-I/O
  # messages can carry addresses the BIOS loaded)
  "$HARNESS" "$rom" "$@" --frames "$frames" --out "$(native "$dir")" --shot-every 0 --no-swi-log --no-core-log \
    --dump-range 0x02200000 "$range" > "$dir/run.log" 2>&1 || echo "harness exit $?" >> "$dir/run.log"
  echo "$frames" > "$dir/frames.txt"
  rm -f "$dir"/frame_*.bmp "$dir/frames.tsv"
}

# Without official in BIOSES the reports compare against the earlier official
# runs in $1 (runs or runs-dsi); refuse before anything is deleted if they
# are missing.
need_official() {
  case " $BIOSES " in *" official "*) return 0 ;; esac
  if [ ! -d "$OUT/$1/official/normal" ]; then
    echo "BIOSES ($BIOSES) has no official and $OUT/$1/official has no earlier runs;" >&2
    echo "add official to BIOSES" >&2
    exit 1
  fi
}

# clear only the runs that are redone
clear_runs() { # runs dir
  mkdir -p "$OUT/$1"
  for bios in $BIOSES; do rm -rf "$OUT/$1/$bios"; done
}

ids() { # manifest: isolated test ids
  "$PYTHON" -I -c "import json,sys; print(' '.join(str(t['id']) for t in json.load(open(sys.argv[1]))['tests'] if t['isolated']))" "$1"
}

for console in $MODES; do
  case $console in
  ds)
    ROM=$(native "$BUILD/swi-testrom.nds")
    MANIFEST=$(native "$BUILD/tests.json")
    [ -f "$BUILD/swi-testrom.nds" ] || { echo "no ROM in $BUILD, run build.sh first" >&2; exit 1; }
    need_official runs
    clear_runs runs
    ISO=$(ids "$MANIFEST")
    SR=$("$PYTHON" -I -c "import json,sys; print(' '.join(str(v['variant']) for v in json.load(open(sys.argv[1]))['softreset']))" "$MANIFEST")
    for bios in $BIOSES; do
      echo "DS $bios: normal"
      run ds "$bios" normal "$NORMAL_FRAMES"
      echo "DS $bios: $(echo "$ISO" | wc -w | tr -d ' ') isolated tests"
      for id in $ISO; do
        run ds "$bios" "iso-$id" "$ISO_FRAMES" --poke 0x02300000=1 --poke 0x02300004="$id"
      done
      echo "DS $bios: SoftReset variants $SR"
      for v in $SR; do
        run ds "$bios" "sr-$v" "$SR_FRAMES" --poke 0x02300000=2 --poke 0x02300004="$v"
      done
    done
    for bios in $BIOSES; do
      [ "$bios" = official ] && continue
      if [ "$bios" = free ]; then rep=report.txt; else rep=report-$bios.txt; fi
      "$PYTHON" -I "$(native "$HERE/report.py")" "$MANIFEST" "$(native "$OUT/runs")" --b "$bios" --out "$(native "$OUT/$rep")"
    done
    ;;
  dsi)
    ROM_TWL=$(native "$BUILD/twl/swi-testrom-twl.nds")
    MANIFEST=$(native "$BUILD/twl/tests.json")
    [ -f "$BUILD/twl/swi-testrom-twl.nds" ] || { echo "no DSi ROM in $BUILD/twl, run build.sh first" >&2; exit 1; }
    [ -f "$DSIDIR/nand.bin" ] || { echo "no nand.bin in $DSIDIR" >&2; exit 1; }
    need_official runs-dsi
    cp "$DSIDIR/nand.bin" "$NAND"
    clear_runs runs-dsi
    ISO=$(ids "$MANIFEST")
    for bios in $BIOSES; do
      echo "DSi $bios: normal"
      run dsi "$bios" normal "$DSI_NORMAL_FRAMES"
      echo "DSi $bios: $(echo "$ISO" | wc -w | tr -d ' ') isolated tests"
      for id in $ISO; do
        run dsi "$bios" "iso-$id" "$ISO_FRAMES" --poke 0x02300000=1 --poke 0x02300004="$id"
      done
    done
    if cmp -s "$DSIDIR/nand.bin" "$NAND"; then echo "DSi: NAND copy unchanged by the runs"
    else echo "DSi: WARNING, the NAND copy changed during the runs" >&2; fi
    rm -f "$NAND"
    for bios in $BIOSES; do
      [ "$bios" = official ] && continue
      if [ "$bios" = free ]; then rep=report-dsi.txt; else rep=report-dsi-$bios.txt; fi
      "$PYTHON" -I "$(native "$HERE/report.py")" "$MANIFEST" "$(native "$OUT/runs-dsi")" --b "$bios" --out "$(native "$OUT/$rep")"
    done
    ;;
  *)
    echo "unknown mode $console in MODES" >&2; exit 1 ;;
  esac
done
