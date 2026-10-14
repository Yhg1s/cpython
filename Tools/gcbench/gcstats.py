"""Collection statistics from gc.callbacks, shared by the benchmarks.

GCMonitor records every collection's duration, generation and number of
objects collected, and summarizes them split into young and full
collections:

* default (GIL) build: generation 2 collections are full ones, the others
  young;
* free-threaded build: every collection is a full one, unless generational
  collection is enabled (PYTHON_GC_GENERATIONAL=1), which reports young
  collections as generation 0 and full ones as generation 2.
"""

import gc
import os
import statistics
import sysconfig
import threading
import time

FREE_THREADED = bool(sysconfig.get_config_var("Py_GIL_DISABLED"))
GENERATIONAL = FREE_THREADED and os.environ.get("PYTHON_GC_GENERATIONAL", "0") not in ("", "0")
MODE = "ft-gen" if GENERATIONAL else "ft" if FREE_THREADED else "gil"
FULL_GENERATION = len(gc.get_threshold()) - 1 if not FREE_THREADED else 2

perf = time.perf_counter


def is_full(generation):
    if FREE_THREADED and not GENERATIONAL:
        return True
    return generation >= FULL_GENERATION


def pct(values, p):
    """The p-th percentile (nearest rank) of values, or 0.0."""
    if not values:
        return 0.0
    values = sorted(values)
    return values[min(len(values) - 1, int(round(p / 100 * (len(values) - 1))))]


class GCMonitor:
    """Install with start(), remove with stop(); summary() reports.

    Durations are measured around the collection on the collecting thread
    (from the "start" to the "stop" callback)."""

    def __init__(self):
        self.events = []            # (seconds, generation, collected)
        self._start = {}
        self._lock = threading.Lock()

    def __call__(self, phase, info):
        tid = threading.get_ident()
        if phase == "start":
            self._start[tid] = perf()
            return
        t = self._start.pop(tid, None)
        if t is None:
            return
        with self._lock:
            self.events.append((perf() - t, info.get("generation", 0),
                                info.get("collected", 0)))

    def start(self):
        gc.callbacks.append(self)
        return self

    def stop(self):
        if self in gc.callbacks:
            gc.callbacks.remove(self)

    @property
    def collected(self):
        return sum(c for _, _, c in self.events)

    @property
    def total(self):
        return sum(d for d, _, _ in self.events)

    def summary(self):
        ms = [d * 1e3 for d, _, _ in self.events]
        young = [d * 1e3 for d, g, _ in self.events if not is_full(g)]
        full = [d * 1e3 for d, g, _ in self.events if is_full(g)]
        r = {
            "gc_mode": MODE,
            "collections": len(ms),
            "young_collections": len(young),
            "full_collections": len(full),
            "gc_s": round(sum(ms) / 1e3, 4),
            "gc_ms_median": round(statistics.median(ms), 3) if ms else 0.0,
            "gc_ms_p99": round(pct(ms, 99), 3),
            "gc_ms_max": round(max(ms), 3) if ms else 0.0,
            "young_ms_median": round(statistics.median(young), 3) if young else 0.0,
            "young_ms_max": round(max(young), 3) if young else 0.0,
            "full_ms_median": round(statistics.median(full), 3) if full else 0.0,
            "full_ms_max": round(max(full), 3) if full else 0.0,
            "full_s": round(sum(full) / 1e3, 4),
            "collected": self.collected,
        }
        return r


def rss_mb():
    """Current (VmRSS) and peak (VmHWM) resident set size in MB."""
    fields = {}
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith(("VmRSS", "VmHWM")):
                key, value = line.split(":")
                fields[key] = int(value.split()[0]) / 1024
    return fields
