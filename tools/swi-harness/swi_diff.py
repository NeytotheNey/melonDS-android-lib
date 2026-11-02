"""Compare two swi-harness runs (for example the official BIOS against FreeBIOS).

    python -I swi_diff.py <run_a_dir> <run_b_dir> [--max N] [--report FILE]

Each run dir holds swi.jsonl and outputs.bin from swi-harness. Calls are
aligned per CPU by call order. Wait-type SWIs (WaitByLoop, IntrWait,
VBlankIntrWait, Halt, CustomHalt) depend on timing, so they are aligned as a
separate stream from the data SWIs. Inside each stream the key of a call is
(SWI, caller address, input registers); when keys stop matching the script
looks ahead for the nearest point where they match again and reports the gap.

For aligned calls it reports differences in documented result registers,
callee-saved registers or flags the SWI changed, and the memory the SWI wrote
(hash, then the first differing bytes from outputs.bin). Differences in r0-r3
that are not documented results are summarized separately as scratch values.
Big cycle-count differences are listed separately, since timing alone is not
a bug.
"""

import argparse
import collections
import json
import os
import statistics
import sys

WAIT_SWIS = {0x03, 0x04, 0x05, 0x06, 0x1F}
LOOKAHEAD = 4000


def load(rundir):
    calls = {9: [], 7: []}
    with open(os.path.join(rundir, "swi.jsonl"), "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            calls[rec["cpu"]].append(rec)
    for cpu in calls:
        calls[cpu].sort(key=lambda r: r["seq"])
    return calls


# input registers each SWI reads (GBATEK); the others may hold leftovers
INPUT_REGS = {
    0x00: [], 0x03: [0], 0x04: [0, 1], 0x05: [], 0x06: [], 0x07: [], 0x08: [0],
    0x09: [0, 1], 0x0B: [0, 1, 2], 0x0C: [0, 1, 2], 0x0D: [0], 0x0E: [0, 1, 2],
    0x0F: [], 0x10: [0, 1, 2], 0x11: [0, 1], 0x12: [0, 1, 2, 3], 0x13: [0, 1, 2, 3],
    0x14: [0, 1], 0x15: [0, 1, 2, 3], 0x16: [0, 1], 0x18: [0, 1], 0x1A: [0],
    0x1B: [0], 0x1C: [0], 0x1D: [], 0x1F: [0, 2],
}

# registers that carry a documented result
RESULT_REGS = {
    0x09: [0, 1, 3], 0x0D: [0], 0x0E: [0], 0x0F: [0], 0x1A: [0], 0x1B: [0],
    0x1C: [0], 0x1D: [0, 1, 2],
}


def used_inputs(rec):
    regs = INPUT_REGS.get(rec["swi"], [0, 1, 2, 3])
    return tuple(rec["in"][n] for n in regs)


def key(rec):
    return (rec["swi"], rec["caller"], used_inputs(rec))


def fmt_call(rec):
    return "%s (0x%02X) seq %d frame %d caller %s in r0-r3 %s" % (
        rec["name"], rec["swi"], rec["seq"], rec["frame"], rec["caller"], " ".join(rec["in"]))


def align(a, b):
    """Yields ('pair', ra, rb) or ('gap', list_a, list_b)."""
    i = j = 0
    while i < len(a) and j < len(b):
        if key(a[i]) == key(b[j]):
            yield ("pair", a[i], b[j])
            i += 1
            j += 1
            continue

        # find the nearest resync point (smallest i'+j')
        best = None
        index_b = {}
        for dj in range(min(LOOKAHEAD, len(b) - j)):
            index_b.setdefault(key(b[j + dj]), dj)
        for di in range(min(LOOKAHEAD, len(a) - i)):
            dj = index_b.get(key(a[i + di]))
            if dj is not None and (best is None or di + dj < best[0] + best[1]):
                best = (di, dj)
                if di + dj <= 1:
                    break
            if best is not None and di > best[0] + best[1]:
                break

        if best is None:
            yield ("gap", a[i:], b[j:])
            return
        di, dj = best
        yield ("gap", a[i:i + di], b[j:j + dj])
        i += di
        j += dj

    if i < len(a) or j < len(b):
        yield ("gap", a[i:], b[j:])


def first_diff_bytes(dir_a, ra, dir_b, rb, context=32):
    if "blob" not in ra or "blob" not in rb:
        return None
    size = min(ra["size"], rb["size"])
    with open(os.path.join(dir_a, "outputs.bin"), "rb") as fa:
        fa.seek(ra["blob"])
        da = fa.read(ra["size"])
    with open(os.path.join(dir_b, "outputs.bin"), "rb") as fb:
        fb.seek(rb["blob"])
        db = fb.read(rb["size"])
    for k in range(size):
        if da[k] != db[k]:
            ndiff = sum(1 for x, y in zip(da, db) if x != y)
            start = max(0, k - 8)
            return {
                "offset": k,
                "address": "%08X" % (int(ra["dst"], 16) + k),
                "differing_bytes": ndiff,
                "a": da[start:k + context].hex(" ").upper(),
                "b": db[start:k + context].hex(" ").upper(),
                "shown_from": start,
            }
    if ra["size"] != rb["size"]:
        return {"offset": size, "note": "sizes differ (%d vs %d)" % (ra["size"], rb["size"])}
    return None


def preserved_problems(rec):
    """Callee-saved registers and flags the SWI changed, within one run."""
    out = []
    if "in_r4_r12" in rec:
        for n in range(9):
            if rec["in_r4_r12"][n] != rec["out_r4_r12"][n]:
                out.append("r%d %s -> %s" % (n + 4, rec["in_r4_r12"][n], rec["out_r4_r12"][n]))
    if rec["cpsr_in"] != rec["cpsr_out"]:
        out.append("cpsr %s -> %s" % (rec["cpsr_in"], rec["cpsr_out"]))
    return out


def compare_pair(ra, rb, dir_a, dir_b):
    """Returns (important differences, scratch register differences)."""
    diffs = []
    scratch = []
    if ra.get("returned", True) != rb.get("returned", True):
        pending = ra if not ra.get("returned", True) else rb
        diffs.append("returned: %s vs %s (a call still waiting when the run stopped, started in frame %d, is "
                     "logged as not returned)" % (ra.get("returned", True), rb.get("returned", True), pending["frame"]))
        return diffs, scratch
    if not ra.get("returned", True):
        return diffs, scratch

    results = RESULT_REGS.get(ra["swi"], [])
    for n in range(4):
        if ra["out"][n] != rb["out"][n]:
            text = "r%d out: %s vs %s" % (n, ra["out"][n], rb["out"][n])
            if n in results:
                diffs.append(text)
            elif ra["in"][n] == rb["in"][n]:
                # leftover values only count when the caller passed the same value
                scratch.append(text)

    pa, pb = preserved_problems(ra), preserved_problems(rb)
    if pa != pb:
        if pa:
            diffs.append("A changed caller state: " + ", ".join(pa))
        if pb:
            diffs.append("B changed caller state: " + ", ".join(pb))

    for field in ("header", "flags_out"):
        if ra.get(field) != rb.get(field):
            diffs.append("%s: %s vs %s" % (field, ra.get(field), rb.get(field)))
    if ra.get("hash") != rb.get("hash") or ra.get("size") != rb.get("size"):
        diffs.append("output %s size %s hash %s vs %s size %s hash %s" % (
            ra.get("dst"), ra.get("size"), ra.get("hash"), rb.get("dst"), rb.get("size"), rb.get("hash")))
        fd = first_diff_bytes(dir_a, ra, dir_b, rb)
        if fd:
            diffs.append("first difference: %s" % json.dumps(fd))
    return diffs, scratch


def summarize_counts(name, calls, out):
    out.append("%s:" % name)
    for cpu in (9, 7):
        c = collections.Counter((r["name"], r["swi"]) for r in calls[cpu])
        nr = collections.Counter(r["name"] for r in calls[cpu] if not r.get("returned", True))
        parts = ["%s(0x%02X) x%d%s" % (n, s, v, (" [%d never returned]" % nr[n]) if nr[n] else "")
                 for (n, s), v in sorted(c.items(), key=lambda kv: kv[0][1])]
        out.append("  ARM%d: %s" % (cpu, ", ".join(parts) if parts else "none"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_a")
    ap.add_argument("run_b")
    ap.add_argument("--max", type=int, default=10, help="divergences to list per CPU and stream")
    ap.add_argument("--report", help="also write the report to this file")
    ap.add_argument("--cycle-ratio", type=float, default=4.0)
    ap.add_argument("--cycle-min", type=int, default=2000)
    args = ap.parse_args()

    a = load(args.run_a)
    b = load(args.run_b)
    name_a = os.path.basename(os.path.normpath(args.run_a))
    name_b = os.path.basename(os.path.normpath(args.run_b))

    out = []
    out.append("A = %s, B = %s" % (args.run_a, args.run_b))
    out.append("")
    out.append("== SWI counts ==")
    summarize_counts("A " + name_a, a, out)
    summarize_counts("B " + name_b, b, out)

    cycle_notes = []

    for cpu in (9, 7):
        for stream_name, pick in (("data", lambda r: r["swi"] not in WAIT_SWIS),
                                  ("wait", lambda r: r["swi"] in WAIT_SWIS)):
            sa = [r for r in a[cpu] if pick(r)]
            sb = [r for r in b[cpu] if pick(r)]
            out.append("")
            out.append("== ARM%d %s SWIs: %d in A, %d in B ==" % (cpu, stream_name, len(sa), len(sb)))

            pairs = 0
            listed = 0
            gaps = 0
            gap_calls_a = gap_calls_b = 0
            mismatched = 0
            cycles_by_swi = collections.defaultdict(lambda: ([], []))
            scratch_kinds = collections.Counter()
            scratch_examples = {}

            for item in align(sa, sb):
                if item[0] == "gap":
                    ga, gb = item[1], item[2]
                    gaps += 1
                    gap_calls_a += len(ga)
                    gap_calls_b += len(gb)
                    if listed < args.max:
                        listed += 1
                        out.append("-- call sequence differs (%d calls only in A, %d only in B)" % (len(ga), len(gb)))
                        for r in ga[:3]:
                            out.append("   A: " + fmt_call(r))
                        if len(ga) > 3:
                            out.append("   A: ... %d more, %s" % (len(ga) - 3, dict(collections.Counter(r["name"] for r in ga))))
                        for r in gb[:3]:
                            out.append("   B: " + fmt_call(r))
                        if len(gb) > 3:
                            out.append("   B: ... %d more, %s" % (len(gb) - 3, dict(collections.Counter(r["name"] for r in gb))))
                    continue

                ra, rb = item[1], item[2]
                pairs += 1
                diffs, scratch = compare_pair(ra, rb, args.run_a, args.run_b)
                if scratch:
                    scratch_kinds[(ra["name"], tuple(x.split(":")[0] for x in scratch))] += 1
                    if (ra["name"], "example") not in scratch_examples:
                        scratch_examples[(ra["name"], "example")] = "%s: %s" % (fmt_call(ra), "; ".join(scratch))
                if diffs:
                    mismatched += 1
                    if listed < args.max:
                        listed += 1
                        out.append("-- output differs: " + fmt_call(ra))
                        out.append("   (B seq %d frame %d)" % (rb["seq"], rb["frame"]))
                        for extra in ("mode", "unpack", "src_head"):
                            if extra in ra:
                                out.append("   %s: %s" % (extra, json.dumps(ra[extra])))
                        for d in diffs:
                            out.append("   " + d)

                if "cycles" in ra and "cycles" in rb:
                    ca, cb = ra["cycles"], rb["cycles"]
                    cycles_by_swi[ra["name"]][0].append(ca)
                    cycles_by_swi[ra["name"]][1].append(cb)
                    hi, lo = max(ca, cb), max(1, min(ca, cb))
                    # Halt/IntrWait/VBlankIntrWait/CustomHalt wait for an IRQ, so their time says nothing
                    timed = ra["swi"] not in WAIT_SWIS or ra["swi"] == 0x03
                    if timed and hi - lo >= args.cycle_min and hi / lo >= args.cycle_ratio and len(cycle_notes) < 200:
                        cycle_notes.append("ARM%d %s: A %d cycles, B %d cycles" % (cpu, fmt_call(ra), ca, cb))

            out.append("   aligned pairs %d, pairs with different results %d, gaps %d (%d calls only in A, %d only in B)" % (
                pairs, mismatched, gaps, gap_calls_a, gap_calls_b))
            for (swi_name, regs), count in sorted(scratch_kinds.items()):
                out.append("   scratch registers (not documented results) differ: %s %s in %d calls" % (
                    swi_name, ", ".join(regs), count))
            for (swi_name, _), text in sorted(scratch_examples.items()):
                out.append("      e.g. " + text)
            for swi_name, (ca, cb) in sorted(cycles_by_swi.items()):
                out.append("   cycles %s: median A %d, B %d; max A %d, B %d" % (
                    swi_name, statistics.median(ca), statistics.median(cb), max(ca), max(cb)))

    out.append("")
    out.append("== big cycle differences (ratio >= %.1f and >= %d cycles apart, IRQ waits excluded), first %d ==" % (
        args.cycle_ratio, args.cycle_min, len(cycle_notes)))
    out.extend("   " + n for n in cycle_notes[:50])

    text = "\n".join(out) + "\n"
    sys.stdout.write(text)
    if args.report:
        with open(args.report, "w") as f:
            f.write(text)


if __name__ == "__main__":
    main()
