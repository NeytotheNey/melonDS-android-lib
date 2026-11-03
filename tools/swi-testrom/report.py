"""Compare SWI test ROM results of the official BIOS and FreeBIOS.

    python -I report.py <tests.json> <runs dir> [--out FILE] [--a official] [--b free]

<runs dir>/<bios>/<mode>/ holds range.bin (the results area, 02200000h, 20000h
bytes, from swi-harness --dump-range), frames.txt and run.log, as written by
run.sh. Results are paired by test id. The report lists, in this order:
  1. mismatches (status, registers, CPSR, kept registers, output hash and
     the first differing output bytes, temp buffer, extras), each with which
     side matches the GBATEK model when there is one,
  2. tests where both sides agree but differ from the GBATEK model,
  3. isolated tests (hangs and crashes) per BIOS,
  4. SoftReset per variant,
  5. cycle differences, on their own since timing alone is not a bug.
"""

import argparse
import collections
import json
import os
import struct
import sys

RES9, RES7 = 0x0000, 0x10000
REC_SIZE = 0x100
MAGIC, DONE = 0x52545753, 0x454E4F44
SR_ID_BASE = 0xF000

W_ID, W_STATUS, W_R0, W_CPSR, W_CHANGED, W_CYCLES, W_HASH, W_OUTLEN, W_MODELOFS, W_TMPHASH = 0, 1, 2, 6, 7, 8, 9, 10, 11, 12
W_EXTRA, W_START, W_WINOFS, W_WIN = 13, 32, 48, 49

SR_FIELDS = (["r%d" % n for n in range(13)] +
             ["sp_sys", "lr_sys", "sp_svc", "lr_svc", "spsr_svc", "sp_irq", "lr_irq", "spsr_irq",
              "CPSR", "CP15 control", "DTCM region", "ITCM region", "Thumb entry",
              "zero words in area 3C00h-3DFFh / 380FC00h-380FDFFh",
              "zero words in area 3E00h-3FFFh / 380FE00h-380FFFFh",
              "pattern words left in the low area", "pattern words left in the high area",
              "ITCM zero words 3C00h-3DFFh", "ITCM zero words 3E00h-3FFFh",
              "DTCM region before", "CP15 control before", "entry address written"])


def load_run(path):
    rb = os.path.join(path, "range.bin")
    run = dict(path=path, frames=None, stop=None, cpus={})
    try:
        run["frames"] = int(open(os.path.join(path, "frames.txt")).read().strip())
    except (OSError, ValueError):
        pass
    try:
        log = open(os.path.join(path, "run.log"), errors="replace").read()
        for line in log.splitlines():
            if line.startswith("emulator stopped, reason") and "reason 1" not in line:
                run["stop"] = line.strip()
    except OSError:
        pass
    if not os.path.exists(rb):
        return run
    data = open(rb, "rb").read()
    for cpu, base in ((9, RES9), (7, RES7)):
        hdr = struct.unpack_from("<64I", data, base)
        c = dict(magic=hdr[0] == MAGIC, count=hdr[4], current=hdr[5], done=hdr[6] == DONE,
                 trap=hdr[8], trap_cpsr=hdr[9], trap_lr=hdr[10], trap_regs=hdr[11:15], recs={})
        if c["magic"]:
            for n in range(min(c["count"], 255)):
                w = struct.unpack_from("<64I", data, base + REC_SIZE * (n + 1))
                c["recs"][w[W_ID]] = w
        run["cpus"][cpu] = c
    return run


def trap_text(c):
    """lr and CPSR of a trap; a BIOS-range lr was only flagged by the ROM"""
    lr = "lr in the BIOS (not stored)" if c["trap"] & (1 << 8) else "lr=%08X" % c["trap_lr"]
    return "%s, CPSR=%08X" % (lr, c["trap_cpsr"])


MASKED = "BIOS-range (not stored)"


def h32(v):
    return "%08X" % v


def rec_bytes(w, first, count):
    return b"".join(struct.pack("<I", x) for x in w[first:first + count])


class Result:
    """One test's outcome on one BIOS."""

    def __init__(self, test, run):
        self.test = test
        self.run = run
        self.rec = None
        self.status = "missing"
        if run is None:
            self.status = "not run"
            return
        c = run["cpus"].get(test["cpu"])
        if c is None or not c["magic"]:
            self.status = "no results header"
            return
        self.rec = c["recs"].get(test["id"])
        if self.rec is not None:
            self.status = "returned"
        else:
            why = []
            if c["current"] == test["id"] and not c["trap"]:
                why.append("didn't finish by frame %s" % run["frames"])
            if c["trap"] & 0xFF == 1:
                why.append("jumped to 0 (ITCM trap: %s)" % trap_text(c))
            elif c["trap"] & 0xFF == 2:
                why.append("CPU exception (debug vector trap: %s)" % trap_text(c))
            if run["stop"]:
                why.append(run["stop"])
            self.status = "; ".join(why) if why else "no record"

    def fields(self):
        """name -> value of everything compared between the two BIOSes."""
        w = self.rec
        t = self.test
        f = collections.OrderedDict()
        if w is None:
            return f
        hash_only = w[W_STATUS] & 2
        if t["kind"] == "table":
            f["result hash (r0)"] = h32(w[W_HASH])
            f["scratch hash (r1-r3)"] = h32(w[W_TMPHASH])
        elif hash_only:
            f["result hash"] = h32(w[W_HASH])
        else:
            for n in range(4):
                f["r%d" % n] = MASKED if w[W_STATUS] & (0x100 << n) else h32(w[W_R0 + n])
        f["CPSR"] = h32(w[W_CPSR])
        changed = [("r%d" % n) for n in range(4, 12) if w[W_CHANGED] & (1 << n)]
        if w[W_CHANGED] & (1 << 13):
            changed.append("sp")
        if w[W_CHANGED] & (1 << 14):
            changed.append("lr")
        f["changed r4-r11/sp/lr"] = ",".join(changed) or "none"
        if not hash_only and w[W_OUTLEN]:
            f["output hash"] = h32(w[W_HASH])
        if t["swi"] == 0x13 and t["kind"] == "callback":
            f["temp buffer hash"] = h32(w[W_TMPHASH])
        for n, name in enumerate(t["extras"]):
            f[name] = h32(w[W_EXTRA + n])
        return f

    def model(self):
        """list of (what, ok) against the GBATEK model."""
        w = self.rec
        e = self.test["expect"]
        out = []
        if w is None:
            return out
        for r, v in e.get("regs", {}).items():
            out.append(("r%s=%08X" % (r, v), w[W_R0 + int(r)] == v))
        if "out" in e:
            ofs = w[W_MODELOFS]
            out.append(("output" if ofs == 0xFFFFFFFF else "output (first difference at +%Xh)" % ofs,
                        ofs == 0xFFFFFFFF))
        for name, v in e.get("extra", {}).items():
            n = self.test["extras"].index(name)
            out.append(("%s=%X" % (name, v), w[W_EXTRA + n] == v))
        for name, (keep, pre) in e.get("extra_changed", {}).items():
            n = self.test["extras"].index(name)
            x = w[W_EXTRA + n]
            out.append(("%s keeps the %08X bits of %08X and changes the rest" % (name, keep, pre),
                        (x & keep) == (pre & keep) and (x & ~keep) != (pre & ~keep)))
        return out

    def complete_model(self):
        """True when the model covers every result: the documented registers
        and, for tests with an output region, the output bytes"""
        t = self.test
        e = t["expect"]
        if t["out_len"] and not t["hash_only"] and "out" not in e:
            return False
        return all(str(r) in e.get("regs", {}) for r in t["doc_regs"])

    def model_text(self):
        m = self.model()
        if not m:
            return "no GBATEK model"
        bad = [what for what, ok in m if not ok]
        if bad:
            return "differs from the GBATEK model: " + ", ".join(bad)
        if self.complete_model():
            return "matches the GBATEK model"
        return "matches the partial GBATEK model (only: %s)" % ", ".join(what for what, _ in m)

    def cycles(self):
        return None if self.rec is None else self.rec[W_CYCLES]


def hexline(b):
    return " ".join("%02X" % x for x in b)


def output_detail(t, ra, rb, la, lb):
    """first differing output bytes, from the stored start and model windows"""
    lines = []
    wa, wb = ra.rec, rb.rec
    sa, sb = rec_bytes(wa, W_START, 16), rec_bytes(wb, W_START, 16)
    n = min(64, wa[W_OUTLEN])
    sa, sb = sa[:n], sb[:n]
    exp = bytes.fromhex(t["expect"]["out"]) if "out" in t["expect"] else None
    diff = next((i for i in range(n) if sa[i] != sb[i]), None)
    if diff is not None:
        lo = max(0, (diff & ~0xF) - 0)
        lines.append("    first difference at +%Xh (dest starts at +%Xh):" % (diff, t["dst_ofs"]))
        lines.append("      %-9s +%03X: %s" % (la, lo, hexline(sa[lo:lo + 16])))
        lines.append("      %-9s +%03X: %s" % (lb, lo, hexline(sb[lo:lo + 16])))
        if exp is not None:
            lines.append("      %-9s +%03X: %s" % ("GBATEK", lo, hexline(exp[lo:lo + 16])))
        return lines
    # beyond the first 64 bytes: show each side's window around its first model difference
    for label, w in ((la, wa), (lb, wb)):
        ofs = w[W_MODELOFS]
        if ofs < 0xFFFFFFFE:
            win = w[W_WINOFS]
            data = rec_bytes(w, W_WIN, 15)[:max(0, min(60, w[W_OUTLEN] - win))]
            lines.append("    %s: first difference from the GBATEK model at +%Xh" % (label, ofs))
            lines.append("      %-9s +%03X: %s" % (label, win, hexline(data[:32])))
            if exp is not None:
                lines.append("      %-9s +%03X: %s" % ("GBATEK", win, hexline(exp[win:win + 32])))
        elif ofs == 0xFFFFFFFF:
            lines.append("    %s: output matches the GBATEK model" % label)
    if not lines:
        lines.append("    (the first 64 bytes are the same; no model to locate the difference)")
    return lines


def documented_regs(t):
    """result registers GBATEK documents for this test (from gen_tests.py)"""
    return set(t["doc_regs"])


def describe(t):
    ins = []
    for v in t["inputs"]:
        ins.append(v if isinstance(v, str) else "%08X" % v)
    s = "#%d ARM%d SWI %02Xh %s: %s" % (t["id"], t["cpu"], t["swi"], t["swi_name"], t["name"])
    return s, "    in: r0-r3 = " + " ".join(ins)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest")
    ap.add_argument("runs")
    ap.add_argument("--out")
    ap.add_argument("--a", default="official")
    ap.add_argument("--b", default="free")
    args = ap.parse_args()
    man = json.load(open(args.manifest))
    tests = man["tests"]
    la, lb = args.a, args.b

    runs = {}
    for bios in (la, lb):
        d = os.path.join(args.runs, bios)
        runs[bios] = {}
        if os.path.isdir(d):
            for name in sorted(os.listdir(d)):
                runs[bios][name] = load_run(os.path.join(d, name))

    def run_for(bios, t):
        return runs[bios].get("iso-%d" % t["id"] if t["isolated"] else "normal")

    out = []
    p = out.append
    pairs = [(t, Result(t, run_for(la, t)), Result(t, run_for(lb, t))) for t in tests]

    p("SWI test ROM report: %s vs %s, DS mode" % (la, lb))
    p("%d tests (%d isolated), %d SoftReset variants" % (len(tests), sum(t["isolated"] for t in tests), len(man["softreset"])))
    for bios in (la, lb):
        n = runs[bios].get("normal")
        if n:
            s = ", ".join("ARM%d %d records%s" % (cpu, len(c["recs"]), ", done" if c["done"] else ", NOT done")
                          for cpu, c in sorted(n["cpus"].items(), reverse=True))
            p("  %s normal run: %s%s" % (bios, s, ("; " + n["stop"]) if n["stop"] else ""))
    p("")

    # 1. mismatches
    mism = []
    scratch = []
    for t, ra, rb in pairs:
        diffs = []
        if ra.status != rb.status:
            diffs.append(("status", ra.status, rb.status))
        fa, fb = ra.fields(), rb.fields()
        sdiffs = []
        doc = documented_regs(t)
        for k in fa:
            if k in fb and fa[k] != fb[k]:
                if (k in ("r0", "r1", "r2", "r3") and int(k[1]) not in doc) or k.startswith("scratch"):
                    sdiffs.append((k, fa[k], fb[k]))
                else:
                    diffs.append((k, fa[k], fb[k]))
        if diffs:
            mism.append((t, ra, rb, diffs + sdiffs))
        elif sdiffs:
            scratch.append((t, sdiffs))
    p("=" * 78)
    p("1. MISMATCHES (%d tests): status, documented result registers, CPSR, kept" % len(mism))
    p("   registers, output, callback use, IRQ state. Scratch registers of these")
    p("   tests are listed here too; tests that differ only there are in 1b.")
    p("=" * 78)
    for t, ra, rb, diffs in mism:
        head, ins = describe(t)
        p(head)
        p(ins)
        for k, a, b in diffs:
            p("    %-22s %s: %s   %s: %s" % (k, la, a, lb, b))
        if ra.rec and rb.rec and ra.rec[W_HASH] != rb.rec[W_HASH] and ra.rec[W_OUTLEN] and not (ra.rec[W_STATUS] & 2):
            for line in output_detail(t, ra, rb, la, lb):
                p(line)
        p("    %s: %s" % (la, ra.model_text() if ra.rec else ra.status))
        p("    %s: %s" % (lb, rb.model_text() if rb.rec else rb.status))
        e = t["expect"]
        if e.get("basis"):
            p("    basis: " + e["basis"])
        if e.get("note"):
            p("    note: " + e["note"])
        p("")

    p("=" * 78)
    p("1b. SCRATCH REGISTERS ONLY (%d tests): r0-r3 that GBATEK gives no result for" % len(scratch))
    p("=" * 78)
    for t, sd in scratch:
        p("  #%-4d ARM%d %-58s %s" % (t["id"], t["cpu"], t["name"][:58],
                                      "  ".join("%s %s/%s" % (k, a, b) for k, a, b in sd)))
    p("")

    # 2. both agree but not with the model
    p("=" * 78)
    p("2. BOTH AGREE, BUT DIFFER FROM THE GBATEK MODEL")
    p("=" * 78)
    n2 = 0
    for t, ra, rb in pairs:
        if ra.rec is None or rb.rec is None or ra.fields() != rb.fields():
            continue
        bad = [what for what, ok in ra.model() if not ok]
        if bad:
            n2 += 1
            head, ins = describe(t)
            p(head)
            p(ins)
            p("    both: " + ", ".join(bad))
            w = ra.rec
            if not (w[W_STATUS] & 2):
                p("    both: r0-r3 = %s" % " ".join(h32(x) for x in w[W_R0:W_R0 + 4]))
            if t["expect"].get("note"):
                p("    note: " + t["expect"]["note"])
    if not n2:
        p("(none)")
    p("")

    # 3. isolated
    p("=" * 78)
    p("3. ISOLATED TESTS (one harness run each)")
    p("=" * 78)
    for t, ra, rb in pairs:
        if not t["isolated"]:
            continue
        head, ins = describe(t)
        p(head)
        for label, r in ((la, ra), (lb, rb)):
            if r.rec is None:
                p("    %-9s %s" % (label, r.status))
            else:
                fa = r.fields()
                p("    %-9s returned; %s; %s" % (label, ", ".join("%s=%s" % kv for kv in fa.items()), r.model_text()))
        if t["expect"].get("note"):
            p("    note: " + t["expect"]["note"])
    p("")

    # 4. SoftReset
    p("=" * 78)
    p("4. SOFTRESET")
    p("=" * 78)
    for v in man["softreset"]:
        p("variant %d: %s" % (v["variant"], v["desc"]))
        vals = {}
        for bios in (la, lb):
            r = runs[bios].get("sr-%d" % v["variant"])
            c = r["cpus"].get(v["cpu"]) if r else None
            rec = c["recs"].get(SR_ID_BASE + v["variant"]) if c else None
            if rec is None:
                why = "no result"
                if c and c["current"] == SR_ID_BASE + v["variant"]:
                    why = "didn't reach the probe by frame %s" % r["frames"]
                if c and c["trap"]:
                    why += "; trap %d, %s" % (c["trap"] & 0xFF, trap_text(c))
                if r and r["stop"]:
                    why += "; " + r["stop"]
                vals[bios] = why
            else:
                w = rec
                vv = list(w[W_R0:W_R0 + 4]) + list(w[W_EXTRA:W_EXTRA + 9])          # r0-r12
                vv += list(w[W_EXTRA + 9:W_EXTRA + 17])                              # banked
                vv += [w[W_CPSR], w[W_EXTRA + 17], w[W_EXTRA + 18]]
                vv += list(w[W_START:W_START + 11])
                masked = w[W_START + 11]    # bit n: SR field n was BIOS-range, not stored
                vals[bios] = ["BIOS-range" if masked & (1 << n) else "%08X" % x for n, x in enumerate(vv)]
        for bios in (la, lb):
            if isinstance(vals[bios], str):
                p("    %s: %s" % (bios, vals[bios]))
        if all(not isinstance(vals[b], str) for b in (la, lb)):
            for n, name in enumerate(SR_FIELDS):
                if v["cpu"] == 7 and ("CP15" in name or "TCM" in name):
                    continue
                a, b = vals[la][n], vals[lb][n]
                mark = "" if a == b else "   <-- differs"
                p("    %-50s %s: %-10s  %s: %-10s%s" % (name, la, a, lb, b, mark))
        elif not isinstance(vals.get(la), str) or not isinstance(vals.get(lb), str):
            for bios in (la, lb):
                if not isinstance(vals[bios], str):
                    p("    %s: " % bios + ", ".join("%s=%s" % (name, vals[bios][n]) for n, name in enumerate(SR_FIELDS)))
        p("")

    # 5. cycles
    p("=" * 78)
    p("5. CYCLE DIFFERENCES (33MHz timer ticks, %s vs %s)" % (la, lb))
    p("=" * 78)
    rows = []
    for t, ra, rb in pairs:
        ca, cb = ra.cycles(), rb.cycles()
        if ca is None or cb is None or ca == cb:
            continue
        rows.append((t, ca, cb))
    by_swi = collections.defaultdict(list)
    for t, ca, cb in rows:
        by_swi[(t["cpu"], t["swi"], t["swi_name"])].append((t, ca, cb))
    p("summary per SWI (tests with different cycles / median ratio %s/%s):" % (lb, la))
    for (cpu, swi, name), lst in sorted(by_swi.items()):
        ratios = sorted(cb / ca for _, ca, cb in lst if ca)
        med = ratios[len(ratios) // 2] if ratios else 0
        p("  ARM%d %02Xh %-24s %3d tests, median %.2fx (min %.2fx, max %.2fx)"
          % (cpu, swi, name, len(lst), med, ratios[0] if ratios else 0, ratios[-1] if ratios else 0))
    p("")
    p("per test:")
    for t, ca, cb in rows:
        p("  #%-4d ARM%d %-62s %10d %10d  %.2fx" % (t["id"], t["cpu"], t["name"][:62], ca, cb, cb / ca if ca else 0))

    text = "\n".join(out) + "\n"
    if args.out:
        with open(args.out, "w", newline="\n") as f:
            f.write(text)
        print("wrote %s: %d mismatches, %d cycle differences" % (args.out, len(mism), len(rows)))
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
