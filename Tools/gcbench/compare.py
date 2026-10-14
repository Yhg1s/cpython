"""Compare suite.py results of two or more configurations.

    python compare.py BASE.jsonl OTHER.jsonl [...] [--names NAME,NAME,...]
                      [--sphinx DIR,DIR,... ...] [--pyperf DIR,DIR,...]

The first file is the baseline. Prints Markdown:

1. For every benchmark, its time or throughput, its memory and its longest
   collection, then its other primary results (the ones README.md says to
   read): the mean over the runs in each file with the range when there is
   more than one run, and the change from the baseline in percent,
   (other - baseline) / baseline. Lower is better unless the result says
   "higher is better".
2. With --sphinx (one realapps.py docs-build output directory per
   configuration, comma-separated, the option repeated for more runs) and
   --pyperf (one realapps.py pyperf output directory per configuration):
   the real-application tier, in the same form.
"""

import argparse
import collections
import json
import math
import os
import statistics as st

# label -> [(key, lower_is_better)]; workloads.py runs say their own.
PRIMARY = {
    "gen_frag sparse": [("final_rss_mb", True), ("gc_ms_max", True)],
    "gen_frag churn": [("wall_s", True), ("full_collections", True), ("full_s", True)],
    "gen_frag churn acyclic": [("wall_s", True), ("full_collections", True), ("full_s", True)],
    "bigcollect": [("final_collect_ms", True), ("rss_before_mb", True), ("rss_after_mb", True)],
    "container": [("build_s", True), ("gc_ms_max", True)],
    "doctree": [("wall_s", True), ("gc_max_ms", True)],
    "migrations": [("peak_rss_mb", True), ("backlog_max", True), ("gc_max_ms", True)],
    "deque": [("backlog_max", True), ("peak_rss_mb", True)],
    "bigbuffers 200KB": [("mb_per_collection", True), ("peak_rss_mb", True)],
    "bigbuffers 4MB": [("mb_per_collection", True), ("peak_rss_mb", True)],
    "event_widgets runaway": [("rss_peak_mb", True), ("ops_per_s", False), ("gc_ms_p99", True)],
}
HIGHER_IS_BETTER = {"ops_per_s"}
MEMORY_KEYS = ("final_rss_mb", "side_peak_mb", "rss_peak_mb", "peak_rss_mb",
               "rss_after_mb", "rss_mb")
# key -> (name, unit)
NAMES = {
    "wall_s": ("run time", "s"), "build_s": ("build time", "s"),
    "ops_per_s": ("throughput (higher is better)", "ops/s"),
    "final_collect_ms": ("final collection", "ms"),
    "final_rss_mb": ("final RSS", "MB"), "side_peak_mb": ("side-buffer peak", "MB"),
    "rss_peak_mb": ("peak RSS", "MB"), "peak_rss_mb": ("peak RSS", "MB"),
    "rss_before_mb": ("RSS before collection", "MB"), "rss_after_mb": ("RSS after collection", "MB"),
    "rss_mb": ("RSS", "MB"),
    "gc_ms_max": ("longest collection", "ms"), "gc_max_ms": ("longest collection", "ms"),
    "gc_ms_p99": ("p99 collection", "ms"),
    "full_collections": ("full collections", ""), "full_s": ("time in full collections", "s"),
    "backlog_max": ("largest garbage backlog", "objects"),
    "mb_per_collection": ("allocated between collections", "MB"),
    "leaked_holders": ("leaked holders", ""),
    "gc_share": ("GC share", "%"),
}


def load(path):
    out = collections.OrderedDict()
    for line in open(path):
        r = json.loads(line)
        if r.get("rc") == 0:
            out.setdefault(r["label"], []).append(r)
    return out


def time_key(recs):
    r = recs[0]
    if r["label"] == "bigcollect":
        return "final_collect_ms"
    for k in ("ops_per_s", "build_s", "wall_s"):
        if k in r:
            return k


def results(label, recs):
    """[(key, lower_is_better)]: time or throughput, memory and longest
    collection first, then the other primary results and the GC share."""
    r = recs[0]
    if label in PRIMARY:
        keys = PRIMARY[label]
    else:
        keys = [(k, k not in HIGHER_IS_BETTER) for k in r.get("primary", [])]
    first = [time_key(recs), next((k for k in MEMORY_KEYS if k in r), None),
             next((k for k in ("gc_ms_max", "gc_max_ms") if k in r), None)]
    out = [(k, k not in HIGHER_IS_BETTER) for k in first if k]
    out += [(k, lower) for k, lower in keys if k not in first]
    if "gc_share" in r and "gc_share" not in dict(out):
        out.append(("gc_share", True))
    return out


def values(recs, key):
    scale = 100 if key == "gc_share" else 1      # shown in percent
    return [r[key] * scale for r in recs if isinstance(r.get(key), (int, float))]


def fmt(x, unit=""):
    if x is None:
        return "-"
    if x == 0:
        s = "0"
    elif isinstance(x, int) or abs(x) >= 100:
        s = f"{x:,.0f}"
    elif abs(x) >= 10:
        s = f"{x:.1f}"
    else:
        s = f"{x:.2f}" if abs(x) >= 0.1 else f"{x:.3g}"
    if unit == "%":
        return s + "%"
    return f"{s} {unit}" if unit else s


def cell(vals, unit=""):
    if not vals:
        return "-"
    m = st.mean(vals)
    if m == 0 and unit == "ms":
        return "none"           # a pause of 0: no collection ran
    if len(vals) > 1 and max(vals) != min(vals):
        return f"{fmt(m, unit)} ({fmt(min(vals))}-{fmt(max(vals))})"
    return fmt(m, unit)


def change(base, new):
    """The change from base to new in percent, signed."""
    if not base or not new:
        return "-"
    x = (new - base) / base * 100
    s = f"{x:+.0f}%" if abs(x) >= 9.95 else f"{x:+.1f}%"
    return "0%" if s in ("+0.0%", "-0.0%") else s


def per_benchmark(data, names):
    base = data[0]
    head = " | ".join(names[:1] + [f"{n} | change" for n in names[1:]])
    print(f"| benchmark | result | {head} |")
    print("|---|---|" + "---|" * (2 * len(names) - 1))
    for label, recs in base.items():
        shown = label
        for key, lower in results(label, recs):
            b = values(recs, key)
            if not b:
                continue
            name, unit = NAMES.get(key, (key, ""))
            if not lower and "higher" not in name:
                name += " (higher is better)"
            cells = [cell(b, unit)]
            for d in data[1:]:
                o = values(d.get(label, []), key)
                cells += [cell(o, unit), change(st.mean(b), st.mean(o)) if o else "-"]
            # the benchmark's name only on its first row
            print(f"| {shown} | {name} | " + " | ".join(cells) + " |")
            shown = ""


def sphinx(groups, names):
    """groups: one list of directories per run, one directory per name."""
    rows = []
    for i in range(len(names)):
        rs = []
        for g in groups:
            p = os.path.join(g[i], "summary.jsonl")
            if os.path.exists(p):
                rs += [json.loads(l) for l in open(p)]
        rows.append(rs)
    if not rows[0]:
        return
    print("\nDocs build (mean and range over runs; change from "
          f"{names[0]}; lower is better):\n")
    head = " | ".join(names[:1] + [f"{n} | change" for n in names[1:]])
    print(f"| result | {head} |")
    print("|---|" + "---|" * (2 * len(names) - 1))
    for title, key, unit, scale in (("time", "wall_s", "s", 1), ("GC share", "gc_share", "%", 100),
                                    ("collections", "collections", "", 1),
                                    ("longest collection", "pause_max_ms", "ms", 1),
                                    ("peak RSS", "maxrss_mb", "MB", 1)):
        vals = [[scale * r[key] for r in rs] for rs in rows]
        cells = [cell(vals[0], unit)]
        for v in vals[1:]:
            cells += [cell(v, unit), change(st.mean(vals[0]), st.mean(v)) if v else "-"]
        print(f"| {title} | " + " | ".join(cells) + " |")


def pyperf_times(path):
    """Median time per benchmark from a pyperf JSON file (without pyperf)."""
    d = json.load(open(path))
    out = {}
    for b in d["benchmarks"]:
        name = b.get("metadata", {}).get("name") or d.get("metadata", {}).get("name")
        vals = [v for r in b["runs"] for v in r.get("values", [])]
        if name and vals:
            out[name] = st.median(vals)
    return out


def pyperf(dirs, names):
    times = [pyperf_times(os.path.join(d, "pyperf.json")) for d in dirs]
    stats = []
    for d in dirs:
        p = os.path.join(d, "summary.jsonl")
        stats.append({r["bench"]: r for r in map(json.loads, open(p))} if os.path.exists(p) else {})
    benches = [b for b in times[0] if all(b in t for t in times)]
    print("\npyperformance (median time; GC share and peak RSS from the statistics hook; "
          f"change from {names[0]}, lower is better):\n")
    head = " | ".join(f"{n} time | GC share | peak RSS" for n in names)
    print(f"| benchmark | {head} | " + " | ".join(f"{n} time change" for n in names[1:]) + " |")
    print("|---|" + "---|" * (3 * len(names) + len(names) - 1))
    ratios = collections.defaultdict(list)
    for b in benches:
        cells = []
        for t, s in zip(times, stats):
            # (the statistics hook names some benchmarks without the variant)
            r = s.get(b) or next((v for k, v in s.items() if b.startswith(k + "_")), {})
            share = f"{100 * r['gc_share']:.1f}%" if "gc_share" in r else "-"
            cells += [fmt(t[b] * 1e3, "ms"), share, fmt(r.get("maxrss_mb"), "MB")]
        for i in range(1, len(names)):
            ratios[i].append(times[i][b] / times[0][b])
            cells.append(change(times[0][b], times[i][b]))
        print(f"| {b} | " + " | ".join(cells) + " |")
    for i in range(1, len(names)):
        g = math.exp(st.mean(map(math.log, ratios[i])))
        print(f"\n{names[i]}: time change from {names[0]}, geometric mean: {change(1, g)}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("files", nargs="+", help="suite.py output, the baseline first")
    p.add_argument("--names", help="comma-separated names (default: the file names)")
    p.add_argument("--sphinx", action="append", default=[],
                   help="comma-separated docs-build output directories, one per configuration; "
                        "repeat for more runs")
    p.add_argument("--pyperf", help="comma-separated pyperf output directories, one per configuration")
    args = p.parse_args()
    names = (args.names.split(",") if args.names
             else [os.path.splitext(os.path.basename(f))[0] for f in args.files])
    data = [load(f) for f in args.files]
    print("Results per benchmark (mean, range over runs; change from "
          f"{names[0]}; lower is better unless the result says otherwise):\n")
    per_benchmark(data, names)
    if args.sphinx:
        sphinx([g.split(",") for g in args.sphinx], names)
    if args.pyperf:
        pyperf(args.pyperf.split(","), names)


if __name__ == "__main__":
    main()
