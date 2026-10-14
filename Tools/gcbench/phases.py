"""Summarise "gcphase" lines printed by a build with GC_PHASE_TRACE.

Reads stderr output from stdin and prints the median of each field over
collections whose candidate count is at least --min-candidates (to skip the
small collections during startup and shutdown).
"""
import argparse
import statistics
import sys

p = argparse.ArgumentParser()
p.add_argument("--min-candidates", type=int, default=100_000)
p.add_argument("--stat", choices=["median", "max", "mean", "sum"], default="median")
p.add_argument("--young", type=int, choices=[0, 1], default=None,
               help="only young (1) or full (0) collections")
args = p.parse_args()

rows = []
for line in sys.stdin:
    if not line.startswith("gcphase "):
        continue
    row = dict(kv.split("=") for kv in line.split()[1:])
    if args.young is not None and int(row.get("young", 0)) != args.young:
        continue
    if int(row["candidates"]) >= args.min_candidates:
        rows.append({k: float(v) for k, v in row.items()})

if not rows:
    sys.exit("no matching collections")
fn = {"median": statistics.median, "max": max, "mean": statistics.mean,
      "sum": lambda it: sum(it)}[args.stat]
print(f"{len(rows)} collections, {args.stat}:")
out = []
for key in rows[0]:
    v = fn(r[key] for r in rows)
    out.append(f"{key}={v:.0f}" if key in ("candidates", "collected", "assisted", "local", "skipdef", "threads", "young", "live")
               else f"{key}={v:.2f}")
print("  " + " ".join(out[:5]))
print("  " + " ".join(out[5:]))
