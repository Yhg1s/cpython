"""Garbage collector benchmarks for default and free-threaded builds.

Subcommands:

  collect   Time gc.collect() when the heap holds N small reference cycles
            of garbage (cost proportional to the amount of garbage).
            Modelled on pyperformance's gc_collect benchmark.

  traverse  Time gc.collect() over a large live object graph that contains
            no garbage (cost proportional to the live heap). Modelled on
            pyperformance's gc_traversal benchmark.

  pause     Multi-threaded steady state: worker threads create cyclic
            garbage while a live heap is kept around. A "ticker" thread
            spins reading the clock and records every gap, so it sees
            stop-the-world pauses the way application threads do. Also
            reports per-collection durations (from gc.callbacks), throughput,
            peak RSS, and the reclamation delay: the time from a sampled
            cycle becoming unreachable to the collector clearing it
            (measured with weakref callbacks).

  release   Memory reclamation: worker threads each build cyclic
            garbage, drop it and go idle; reports RSS after collecting while
            the workers idle, and after they exit.

Every subcommand prints a human readable summary and, with --json, a single
JSON object on the last line.
"""

import argparse
import gc
import json
import os
import random
import statistics
import sys
import sysconfig
import threading
import time
import weakref

FREE_THREADED = bool(sysconfig.get_config_var("Py_GIL_DISABLED"))
perf_ns = time.perf_counter_ns


class Node:
    def __init__(self):
        self.next = None
        self.payload = None


class Rec:
    def __init__(self, ident):
        self.ident = ident
        self.children = []
        self.meta = {"id": ident}


def build_live_heap(n_objects, fragment, seed=1):
    """Build a live graph of about n_objects GC-tracked objects.

    Each Rec contributes three tracked objects (instance, list, dict) and
    links to two random other records, so marking it touches memory in an
    unpredictable order. With fragment=k, k times as many records are
    allocated and only a random 1/k kept, which leaves mimalloc pages
    sparsely occupied the way a long-running process's heap tends to be.
    """
    rng = random.Random(seed)
    n_recs = max(1, n_objects // 3)
    allocated = [Rec(i) for i in range(n_recs * fragment)]
    if fragment > 1:
        rng.shuffle(allocated)
        recs = allocated[:n_recs]
        del allocated
        recs.sort(key=lambda r: r.ident)
    else:
        recs = allocated
    for r in recs:
        r.children.append(recs[rng.randrange(n_recs)])
        r.children.append(recs[rng.randrange(n_recs)])
    return recs


def make_cycles(n):
    for _ in range(n):
        a = Node()
        b = Node()
        c = Node()
        a.next = b
        b.next = c
        c.next = a


def run_at_depth(depth, fn):
    """Call fn() `depth` Python frames deep, each frame holding a few objects
    in locals (the collector scans every frame of every thread's stack)."""
    if depth <= 0:
        return fn()
    items = [depth, str(depth)]
    table = {"depth": depth, "items": items}
    pair = (items, table)
    return run_at_depth(depth - 1, fn)


def rss_kb():
    fields = {}
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith(("VmRSS", "VmHWM")):
                key, value = line.split(":")
                fields[key] = int(value.split()[0])
    return fields


def pct(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    k = min(len(values) - 1, int(round(p / 100 * (len(values) - 1))))
    return values[k]


def ms(ns):
    return ns / 1e6


def cmd_collect(args):
    gc.collect()
    gc.disable()
    times = []
    for _ in range(args.repeat):
        make_cycles(args.cycles)
        t0 = perf_ns()
        n = gc.collect()
        times.append(perf_ns() - t0)
    gc.enable()
    result = {
        "bench": "collect", "cycles": args.cycles, "collected": n,
        "min_ms": ms(min(times)), "median_ms": ms(statistics.median(times)),
    }
    print(f"collect {args.cycles} cycles ({n} objs): "
          f"min {result['min_ms']:.2f} ms, median {result['median_ms']:.2f} ms")
    return result


def cmd_traverse(args):
    gc.disable()
    live = build_live_heap(args.live, args.fragment)
    gc.collect()
    times = []
    for _ in range(args.repeat):
        t0 = perf_ns()
        gc.collect()
        times.append(perf_ns() - t0)
    result = {
        "bench": "traverse", "live": args.live, "fragment": args.fragment,
        "min_ms": ms(min(times)), "median_ms": ms(statistics.median(times)),
        "rss_kb": rss_kb()["VmRSS"],
    }
    gc.enable()
    print(f"traverse live={args.live} fragment={args.fragment}: "
          f"min {result['min_ms']:.2f} ms, median {result['median_ms']:.2f} ms")
    del live
    return result


def cmd_pause(args):
    live = build_live_heap(args.live, args.fragment)
    gc.collect()

    stop = threading.Event()
    gc_events = []          # (start_ns, duration_s, collected)
    gc_start = [0]

    def on_gc(phase, info):
        if phase == "start":
            gc_start[0] = perf_ns()
        else:
            gc_events.append((gc_start[0], info["duration"],
                              info["collected"]))

    gc.callbacks.append(on_gc)

    delays = []             # reclamation delay samples, ns
    counts = [0] * args.threads

    def worker(idx):
        probes = set()

        def on_clear(wr, probes=probes):
            delays.append(perf_ns() - wr.t_dropped)
            probes.discard(wr)

        n = 0
        sample = args.sample_every
        payload = args.payload
        while not stop.is_set():
            for _ in range(200):
                a = Node()
                b = Node()
                a.next = b
                b.next = a
                if payload:
                    a.payload = [n, n]
                n += 1
                if sample and n % sample == 0:
                    # The cycle becomes unreachable when `a` and `b` are
                    # rebound on the next iteration, microseconds from now.
                    probes.add(_Probe(a, on_clear))
        counts[idx] = n

    gaps = []
    ticks = [0]

    def ticker():
        last = perf_ns()
        threshold = args.gap_threshold_us * 1000
        n = 0
        while not stop.is_set():
            for _ in range(1000):
                now = perf_ns()
                if now - last > threshold:
                    gaps.append((last, now - last))
                last = now
            n += 1000
        ticks[0] = n

    if args.stack_depth:
        sys.setrecursionlimit(args.stack_depth + 1000)
    threads = [threading.Thread(
                   target=run_at_depth, args=(args.stack_depth,
                                              lambda i=i: worker(i)))
               for i in range(args.threads)]
    if not args.no_ticker:
        threads.append(threading.Thread(target=ticker))
    t_start = perf_ns()
    for t in threads:
        t.start()
    time.sleep(args.duration)
    stop.set()
    for t in threads:
        t.join()
    elapsed = (perf_ns() - t_start) / 1e9
    gc.callbacks.remove(on_gc)
    mem = rss_kb()

    if args.events:
        for start, dur, collected in gc_events:
            print(f"  gc at {(start - t_start) / 1e6:8.1f} ms: "
                  f"{dur * 1e3:7.1f} ms, collected {collected}")
    gc_durs = [d * 1e9 for _, d, _ in gc_events]
    gap_ns = [g for _, g in gaps]
    result = {
        "bench": "pause", "free_threaded": FREE_THREADED,
        "threads": args.threads, "live": args.live,
        "fragment": args.fragment, "duration_s": elapsed,
        "cycles_per_s": sum(counts) / elapsed,
        "collections": len(gc_events),
        "gc_ms_total": ms(sum(gc_durs)),
        "gc_ms_median": ms(statistics.median(gc_durs)) if gc_durs else 0,
        "gc_ms_max": ms(max(gc_durs)) if gc_durs else 0,
        "collected_median": (statistics.median(c for *_, c in gc_events)
                             if gc_events else 0),
        "tick_gaps": len(gap_ns),
        "gap_ms_p50": ms(pct(gap_ns, 50)),
        "gap_ms_p99": ms(pct(gap_ns, 99)),
        "gap_ms_max": ms(max(gap_ns)) if gap_ns else 0,
        "gap_ms_total": ms(sum(gap_ns)),
        "reclaim_samples": len(delays),
        "reclaim_ms_p50": ms(pct(delays, 50)),
        "reclaim_ms_p99": ms(pct(delays, 99)),
        "reclaim_ms_max": ms(max(delays)) if delays else 0,
        "rss_mb": mem["VmRSS"] / 1024, "peak_rss_mb": mem["VmHWM"] / 1024,
        "stack_depth": args.stack_depth,
    }
    print(f"pause: {args.threads} workers, live={args.live}, "
          f"fragment={args.fragment}, {'FT' if FREE_THREADED else 'GIL'}")
    print(f"  throughput      {result['cycles_per_s']/1e6:.2f} M cycles/s")
    print(f"  collections     {result['collections']}  "
          f"(median {result['gc_ms_median']:.1f} ms, "
          f"max {result['gc_ms_max']:.1f} ms, "
          f"total {result['gc_ms_total']:.0f} ms, "
          f"median collected {result['collected_median']:.0f})")
    print(f"  ticker gaps >{args.gap_threshold_us}us  {result['tick_gaps']}  "
          f"(p50 {result['gap_ms_p50']:.2f} ms, p99 {result['gap_ms_p99']:.2f}"
          f" ms, max {result['gap_ms_max']:.2f} ms, "
          f"total {result['gap_ms_total']:.0f} ms)")
    print(f"  reclaim delay   p50 {result['reclaim_ms_p50']:.1f} ms, "
          f"p99 {result['reclaim_ms_p99']:.1f} ms, "
          f"max {result['reclaim_ms_max']:.1f} ms "
          f"({result['reclaim_samples']} samples)")
    print(f"  rss             {result['rss_mb']:.0f} MB "
          f"(peak {result['peak_rss_mb']:.0f} MB)")
    del live
    return result


def cmd_release(args):
    def rss_mb():
        return rss_kb()["VmRSS"] / 1024

    base = rss_mb()
    built = threading.Barrier(args.threads + 1)
    dropped = threading.Barrier(args.threads + 1)
    finish = threading.Event()

    def worker():
        keep = []
        for _ in range(args.objects // 3):
            a = Node()
            b = Node()
            a.next = b
            b.next = a
            a.payload = [None]
            keep.append(a)
        built.wait()
        del keep, a, b
        dropped.wait()
        finish.wait()   # idle: no allocations until told to exit

    threads = [threading.Thread(target=worker) for _ in range(args.threads)]
    for t in threads:
        t.start()
    built.wait()
    peak = rss_mb()
    dropped.wait()
    t0 = perf_ns()
    gc.collect()
    collect_ms = ms(perf_ns() - t0)
    after_collect = rss_mb()
    time.sleep(args.idle)
    after_idle = rss_mb()
    finish.set()
    for t in threads:
        t.join()
    gc.collect()
    after_exit = rss_mb()
    result = {
        "bench": "release", "threads": args.threads, "objects": args.objects,
        "base_mb": base, "peak_mb": peak, "collect_ms": collect_ms,
        "after_collect_mb": after_collect, "after_idle_mb": after_idle,
        "after_exit_mb": after_exit,
    }
    print(f"release {args.threads} threads x {args.objects} objects: "
          f"base {base:.0f} MB, peak {peak:.0f} MB, "
          f"after collect {after_collect:.0f} MB ({collect_ms:.0f} ms), "
          f"after {args.idle:g}s idle {after_idle:.0f} MB, "
          f"after threads exit {after_exit:.0f} MB")
    return result


class _Probe(weakref.ref):
    """A weakref that remembers when its referent was dropped."""
    def __new__(cls, obj, callback):
        self = super().__new__(cls, obj, callback)
        self.t_dropped = perf_ns()
        return self

    def __init__(self, obj, callback):
        super().__init__(obj, callback)

    def __hash__(self):
        return id(self)

    def __eq__(self, other):
        return self is other


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--json", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("collect")
    s.add_argument("--cycles", type=int, default=100_000)
    s.add_argument("--repeat", type=int, default=10)

    s = sub.add_parser("traverse")
    s.add_argument("--live", type=int, default=1_000_000)
    s.add_argument("--fragment", type=int, default=1)
    s.add_argument("--repeat", type=int, default=10)

    s = sub.add_parser("pause")
    s.add_argument("--threads", type=int, default=4)
    s.add_argument("--live", type=int, default=1_000_000)
    s.add_argument("--fragment", type=int, default=1)
    s.add_argument("--duration", type=float, default=5.0)
    s.add_argument("--sample-every", type=int, default=997,
                   help="probe one cycle in this many (0 disables probes)")
    s.add_argument("--gap-threshold-us", type=int, default=200)
    s.add_argument("--stack-depth", type=int, default=0,
                   help="run each worker this many Python frames deep")
    s.add_argument("--no-ticker", action="store_true")
    s.add_argument("--events", action="store_true",
                   help="print every collection's start time and duration")
    s.add_argument("--no-payload", dest="payload", action="store_false",
                   help="don't attach a list of ints to each cycle")

    s = sub.add_parser("release")
    s.add_argument("--threads", type=int, default=4)
    s.add_argument("--objects", type=int, default=1_500_000,
                   help="objects per thread")
    s.add_argument("--idle", type=float, default=1.0)

    args = p.parse_args()
    result = {"collect": cmd_collect, "traverse": cmd_traverse,
              "pause": cmd_pause, "release": cmd_release}[args.cmd](args)
    if args.json:
        print(json.dumps(result))


if __name__ == "__main__":
    main()
