"""Cyclic garbage that holds large buffers from Python's own allocator.

    python bigbuffers.py SIZE_BYTES [TOTAL_MB] [--json]

Makes two-object cycles that each hold a bytearray of SIZE_BYTES until
TOTAL_MB (default 2000) have been allocated. The collector counts objects,
so a few objects with large buffers can hold a lot of memory as garbage
before a collection runs: the megabytes allocated per collection and the
peak RSS show how much. (ext_buffers_pymem in workloads.py is the same
question with many small buffers and a live heap next to them.)
"""
import argparse
import gc
import json
import resource
import time

p = argparse.ArgumentParser()
p.add_argument("size", type=int)
p.add_argument("total_mb", type=int, nargs="?", default=2000)
p.add_argument("--json", action="store_true")
args = p.parse_args()


class Node:
    __slots__ = ("other", "buf")


def collections():
    return sum(s["collections"] for s in gc.get_stats())


longest = [0.0]
started = [0.0]


def callback(phase, info):
    if phase == "start":
        started[0] = time.perf_counter()
    else:
        longest[0] = max(longest[0], time.perf_counter() - started[0])


gc.callbacks.append(callback)
count = (args.total_mb << 20) // args.size
n0 = collections()
t0 = time.perf_counter()
for _ in range(count):
    a, b = Node(), Node()
    a.other, b.other = b, a
    a.buf = bytearray(args.size)
wall = time.perf_counter() - t0
n = collections() - n0
result = {
    "size": args.size, "total_mb": args.total_mb, "wall_s": wall,
    "collections": n, "mb_per_collection": args.total_mb / max(n, 1),
    "peak_rss_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024,
    "gc_ms_max": longest[0] * 1e3,
}
if args.json:
    print(json.dumps(result))
else:
    print(f"size={args.size} wall={wall:.2f}s collections={n} "
          f"MB/collection={result['mb_per_collection']:.1f} "
          f"peak RSS={result['peak_rss_mb']:.0f} MB longest={result['gc_ms_max']:.1f} ms")
