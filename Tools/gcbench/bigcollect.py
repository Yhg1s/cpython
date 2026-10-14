"""One large collection, and a large container built while young.

    python bigcollect.py collect [--objects N] [--json]
    python bigcollect.py container [--objects N] [--json]

  collect    Next to 200,000 live objects, build N (default 3,000,000)
             self-referencing objects in a list, drop the list and collect
             once. Reports the build phase and the final collection
             separately (time, collections during the build, longest
             pause), and the current RSS (VmRSS) before and after the final
             collection: whether the memory of the garbage is returned.
  container  The build phase of collect on its own: one list filled with N
             new objects next to the program's existing state, as when
             loading rows or building an index. The list is a single large
             container that is new while it is being filled; if it stays
             young, every young collection traverses all it holds, so young
             pauses grow with it. (Where generations are kept per page,
             whether it stays young depends on what else shares its page,
             so slightly different configurations may not reproduce it.)
             Reports the build time and the young and full pauses.
"""

import argparse
import gc
import json
import sys
import time

from gcstats import GCMonitor, rss_mb

perf = time.perf_counter


class A:
    pass


def bench_collect(n):
    keep = [A() for _ in range(200_000)]
    gc.collect()
    mon = GCMonitor().start()
    sys.audit("gcbench.measure", "start")
    t0 = perf()
    l = []
    for i in range(n):
        a = A(); a.s = a; l.append(a)
    build_s = perf() - t0
    build = mon.summary()
    before = rss_mb()["VmRSS"]
    del l
    t1 = perf()
    collected = gc.collect()
    final_ms = (perf() - t1) * 1e3
    sys.audit("gcbench.measure", "stop")
    mon.stop()
    after = rss_mb()["VmRSS"]
    del keep
    return {"bench": "bigcollect", "objects": n,
            "rss_before_mb": round(before), "rss_after_mb": round(after),
            "final_collect_ms": round(final_ms, 1), "final_collected": collected,
            "build_s": round(build_s, 3), "build_gc_s": build["gc_s"],
            "build_collections": build["collections"],
            "build_young": build["young_collections"], "build_full": build["full_collections"],
            "build_gc_ms_max": build["gc_ms_max"], "gc_mode": build["gc_mode"]}


def bench_container(n):
    # collect's build phase on its own: the program's existing state, then
    # one list filled with n new objects (each in a small cycle).
    keep = [A() for _ in range(200_000)]
    gc.collect()
    mon = GCMonitor().start()
    sys.audit("gcbench.measure", "start")
    t0 = perf()
    rows = []
    for i in range(n):
        a = A(); a.s = a; rows.append(a)
    build_s = perf() - t0
    sys.audit("gcbench.measure", "stop")
    mon.stop()
    s = mon.summary()
    del keep, rows
    return {"bench": "container", "objects": n, "build_s": round(build_s, 3),
            "rss_mb": round(rss_mb()["VmRSS"]), **s}


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=("collect", "container"))
    p.add_argument("--objects", type=int, default=3_000_000)
    p.add_argument("--json", action="store_true")
    args = p.parse_args()
    if args.mode == "collect":
        r = bench_collect(args.objects)
        print(f"collect: {r['objects']} objects, final collection {r['final_collect_ms']:.0f} ms, "
              f"RSS {r['rss_before_mb']} -> {r['rss_after_mb']} MB; build {r['build_s']:.2f}s "
              f"with {r['build_collections']} collections ({r['build_gc_s']:.2f}s, "
              f"max {r['build_gc_ms_max']:.1f} ms)")
    else:
        r = bench_container(args.objects)
        print(f"container: {r['objects']} objects in {r['build_s']:.2f}s, "
              f"{r['young_collections']} young (median {r['young_ms_median']:.2f}, max "
              f"{r['young_ms_max']:.1f} ms), {r['full_collections']} full (max "
              f"{r['full_ms_max']:.1f} ms), gc {r['gc_s']:.2f}s")
    if args.json:
        print(json.dumps(r))


if __name__ == "__main__":
    main()
