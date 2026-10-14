"""Per-process GC statistics for real applications (realapps.py).

Active only when GCBENCH_STATS_DIR is set; put this directory on PYTHONPATH.
Records every collection (duration, generation, objects collected, and the
extra instrumentation keys of builds that report them), the process's wall
and CPU time and its peak RSS, and at exit appends one JSON line to
GCBENCH_STATS_DIR/gcstats-<pid>.jsonl. Rows are kept in an array, not in
GC-tracked lists, so that recording doesn't change what the collector sees.
"""
import os

_dir = os.environ.get("GCBENCH_STATS_DIR")
if _dir:
    import array
    import atexit
    import gc
    import json
    import sys
    import sysconfig
    import threading
    import time

    _t0 = time.perf_counter()
    _c0 = time.process_time()
    _start = {}
    _rows = array.array("q")      # us, generation, collected
    _ROW = 3

    def _cb(phase, info):
        now = time.perf_counter()
        tid = threading.get_ident()
        if phase == "start":
            _start[tid] = now
            return
        s = _start.pop(tid, None)
        if s is not None:
            _rows.extend((round((now - s) * 1e6), info.get("generation", 0),
                          info.get("collected", 0)))

    gc.callbacks.append(_cb)

    def _dump():
        ft = bool(sysconfig.get_config_var("Py_GIL_DISABLED"))
        gen = ft and os.environ.get("PYTHON_GC_GENERATIONAL", "0") not in ("", "0")
        rec = {
            "argv": sys.argv,
            "pid": os.getpid(),
            "gc_mode": "ft-gen" if gen else "ft" if ft else "gil",
            "wall_s": time.perf_counter() - _t0,
            "cpu_s": time.process_time() - _c0,
            "colls": [_rows[i:i + _ROW].tolist() for i in range(0, len(_rows), _ROW)],
        }
        try:
            import resource
            rec["maxrss_mb"] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024
        except Exception:
            pass
        try:
            with open(os.path.join(_dir, "gcstats-%d.jsonl" % os.getpid()), "a") as f:
                f.write(json.dumps(rec) + "\n")
        except OSError:
            pass

    atexit.register(_dump)
