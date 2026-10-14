"""Summarise `perf script` output for samples taken inside the GC.

    perf script -i perf.data | python3 perfgc.py [--root FUNC] [--top N]

Keeps samples whose call chain contains --root (default gc_collect_main),
and prints the hottest leaf functions and the hottest GC phase functions
(the frame nearest the root among the known phase functions).
"""
import argparse
import collections
import re
import sys

PHASES = [
    "gc_mark_alive_from_roots", "update_refs", "deduce_unreachable_heap",
    "mark_heap_object", "scan_heap_object", "delete_garbage_chunk",
    "delete_garbage_parallel", "delete_garbage", "gc_assist_delete",
    "_PyMem_TrimHeaps", "_PyMem_TrimArenas", "process_delayed_frees",
    "merge_queued_objects", "finalize_garbage", "handle_resurrected_objects",
    "find_weakref_callbacks", "stop_the_world", "_PyEval_StopTheWorld",
    "mi_heap_visit_blocks", "mi_visit_batch_flush",
]

p = argparse.ArgumentParser()
p.add_argument("--root", default="gc_collect_main")
p.add_argument("--top", type=int, default=20)
args = p.parse_args()

leaf = collections.Counter()
phase = collections.Counter()
total = kept = 0
frames = []

def flush():
    global total, kept
    if not frames:
        return
    total += 1
    names = [f for f in frames]
    if any(n == args.root for n in names):
        kept += 1
        leaf[names[0]] += 1
        # innermost-first; find the outermost known phase below the root
        chosen = None
        for n in names:
            if n == args.root:
                break
            if n in PHASES:
                chosen = n
        phase[chosen or "(other)"] += 1

sym = re.compile(r"^\s+[0-9a-f]+\s+(\S+)")
for line in sys.stdin:
    if not line.strip():
        flush()
        frames = []
        continue
    m = sym.match(line)
    if m:
        name = m.group(1).split("+")[0]
        frames.append(name)
flush()

print(f"{kept} of {total} samples under {args.root}")
print("\nleaf functions:")
for n, c in leaf.most_common(args.top):
    print(f"  {100 * c / kept:5.1f}%  {n}")
print("\nphase (outermost known function below the root):")
for n, c in phase.most_common(args.top):
    print(f"  {100 * c / kept:5.1f}%  {n}")
