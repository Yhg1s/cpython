"""Page occupancy scenarios for generational collection.

    python gen_frag.py {sparse|churn} [N] [--acyclic F] [--json]

  sparse  1 in 20 new objects is kept for good (sparse long-lived survivors
          scattered over young pages), the rest become cyclic garbage.
  churn   A long-lived table of 200,000 objects whose entries are replaced
          at random N times: each object lives long enough to be promoted
          and then dies in the old generation, like a cache or session
          table with turnover. --acyclic F makes a fraction F of the
          replacement objects acyclic (freed by reference counting when
          replaced), as most real objects are.

N defaults to 3,000,000. The primary result is the final RSS (memory
stranded on sparse pages); the line also reports the time, an RSS trace,
and the collections (young and full) with their total time.
"""

import argparse
import gc
import json
import random
import sys
import time

from gcstats import GCMonitor, rss_mb


class Obj:
    __slots__ = ("a", "b", "c", "__weakref__")


def rss():
    return rss_mb()["VmRSS"]


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=("sparse", "churn"))
    p.add_argument("n", type=int, nargs="?", default=3_000_000)
    p.add_argument("--acyclic", type=float, default=0.0,
                   help="churn: fraction of replacement objects without a cycle")
    p.add_argument("--json", action="store_true")
    args = p.parse_args()
    mode, n = args.mode, args.n

    rng = random.Random(0)
    mon = GCMonitor().start()
    # Marks the measured part for external GC statistics (an audit hook).
    sys.audit("gcbench.measure", "start")
    t0 = time.perf_counter()
    samples = []
    if mode == "sparse":
        keep = []
        for i in range(n):
            o = Obj(); o.a = o; o.b = [i]; o.c = None      # cycle
            if i % 20 == 0:
                keep.append(o)
            if i % (n // 10) == 0:
                samples.append(round(rss()))
        live = len(keep)
    else:
        table = {i: Obj() for i in range(200_000)}
        for o in table.values():
            o.a = o; o.b = [0]
        acyclic = args.acyclic
        for i in range(n):
            k = rng.randrange(200_000)
            o = Obj(); o.b = [i]
            if rng.random() >= acyclic:
                o.a = o                                    # cycle
            table[k] = o                                   # old entry becomes garbage
            if i % (n // 10) == 0:
                samples.append(round(rss()))
        live = len(table)
    gc.collect()
    elapsed = time.perf_counter() - t0
    sys.audit("gcbench.measure", "stop")
    mon.stop()
    s = mon.summary()
    final = round(rss())
    print(f"{mode}: time {elapsed:.2f}s live={live} rss over run {samples} final {final} MB"
          f"  gc {s['gc_s']:.2f}s in {s['collections']} collections"
          f" ({s['young_collections']} young, {s['full_collections']} full {s['full_s']:.2f}s,"
          f" max {s['gc_ms_max']:.1f} ms)")
    if args.json:
        print(json.dumps({"bench": f"gen_frag_{mode}", "n": n, "acyclic": args.acyclic,
                          "final_rss_mb": final, "wall_s": round(elapsed, 3),
                          "live": live, "rss_trace_mb": samples, **s}))


if __name__ == "__main__":
    main()
