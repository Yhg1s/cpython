"""Run the recommended gcbench set and record one JSON line per run.

    python suite.py --python PY [--env K=V ...] [--tier T ...] [--repeat N]
                    [--quick] [--out FILE]

Every benchmark runs in its own process, pinned with taskset: the
single-threaded ones to two performance cores (--cpus, default 2,3), the
multi-threaded ones to as many cores as they have threads (--thread-cpus,
default 0-7), or all of them to --all-cpus (for collectors with helper
threads, which need spare cores). Run one suite at a time on an otherwise idle
machine.

Tiers:

  pathology  Targeted tests of known collector problems (stress tests; their
             GC share is far above real programs'). Read the primary result
             of each: final RSS for gen_frag sparse, time and full
             collections for churn, RSS after the collection and the young
             pauses for bigcollect, peak RSS and longest pause for
             migrations, GC time per document against heap size for
             doctree, backlog and peak RSS for deque, side memory for
             ext_buffers_malloc and _pymem, c_globals against py_globals, and peak RSS for
             event_widgets runaway (4 threads making cyclic garbage faster
             than collections keep up with: the heap grows by gigabytes a
             second, so it runs for 4 s).
  workloads  Steady-state workloads run long enough to reach full
             collections: web_service (realistic proportions), the
             event/ORM/DOM family represented by orm_session with mostly
             acyclic garbage and a medium-lived component, async_server, and
             json_churn as the no-GC control.
  threads    web_service on 4 and 8 threads and orm_session (mostly acyclic
             garbage) on 4 threads, each thread on its own core.

--quick shortens every run (for a smoke test, not for measurements).
"""

import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def entries(quick):
    d = (lambda s: max(2, s // 10)) if quick else (lambda s: s)
    n = (lambda x: max(1, x // 10)) if quick else (lambda x: x)
    wl = lambda name, secs, threads=1, *extra: (
        ["workloads.py", "run", name, "--threads", str(threads), "--duration", str(d(secs)),
         "--no-ticker", "--json", *extra])
    return [
        # (tier, label, argv, threads)
        ("pathology", "gen_frag sparse", ["gen_frag.py", "sparse", str(n(3_000_000)), "--json"], 1),
        ("pathology", "gen_frag churn", ["gen_frag.py", "churn", str(n(3_000_000)), "--json"], 1),
        ("pathology", "gen_frag churn acyclic", ["gen_frag.py", "churn", str(n(3_000_000)),
                                                 "--acyclic", "0.9", "--json"], 1),
        ("pathology", "bigcollect", ["bigcollect.py", "collect", "--json"], 1),
        ("pathology", "container", ["bigcollect.py", "container", "--json"], 1),
        ("pathology", "doctree", ["revert_cases.py", "doctree", "--docs", str(n(4800))], 1),
        ("pathology", "migrations", ["revert_cases.py", "migrations", "--steps", str(n(300))], 1),
        ("pathology", "deque", ["revert_cases.py", "deque", "--iterations", str(n(1_500_000))], 1),
        ("pathology", "bigbuffers 200KB", ["bigbuffers.py", "200000", str(n(2000)), "--json"], 1),
        ("pathology", "bigbuffers 4MB", ["bigbuffers.py", "4000000", str(n(2000)), "--json"], 1),
        ("pathology", "ext_buffers_malloc", wl("ext_buffers_malloc", 30), 1),
        ("pathology", "ext_buffers_pymem", wl("ext_buffers_pymem", 30), 1),
        ("pathology", "hidden_refs", ["workloads.py", "run", "hidden_refs", "--threads", "1",
                                      "--ops", str(n(20_000_000)), "--no-ticker", "--json"], 1),
        ("pathology", "c_globals", wl("c_globals", 30), 1),
        ("pathology", "py_globals", wl("py_globals", 30), 1),
        # Garbage arrives faster than a collection can process it: the heap
        # grows by about 1.7 GB a second (4 threads on 4 cores), so the run
        # is kept short.
        ("pathology", "event_widgets runaway", wl("event_widgets", 4, 4), 4),
        ("workloads", "web_service", wl("web_service", 60), 1),
        ("workloads", "orm_session mixed", wl("orm_session", 60, 1, "--acyclic", "0.95",
                                              "--medium", "2000"), 1),
        ("workloads", "async_server", wl("async_server", 30), 1),
        ("workloads", "json_churn", wl("json_churn", 30), 1),
        ("threads", "web_service 4t", wl("web_service", 60, 4), 4),
        ("threads", "web_service 8t", wl("web_service", 60, 8), 8),
        ("threads", "orm_session mixed 4t", wl("orm_session", 60, 4, "--acyclic", "0.95",
                                                 "--medium", "2000"), 4),
    ]


def cpus_for(threads, args):
    if args.all_cpus:
        return args.all_cpus
    if threads <= 1:
        return args.cpus
    first = int(args.thread_cpus.split("-")[0])
    return f"{first}-{first + threads - 1}"


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--python", default=sys.executable)
    p.add_argument("--env", action="append", default=[], metavar="K=V",
                   help="environment for the benchmarks, e.g. PYTHON_GC_GENERATIONAL=1")
    p.add_argument("--tier", action="append", choices=("pathology", "workloads", "threads"))
    p.add_argument("--only", action="append", help="run only these labels")
    p.add_argument("--repeat", type=int, default=1)
    p.add_argument("--cpus", default="2,3")
    p.add_argument("--thread-cpus", default="0-7")
    p.add_argument("--all-cpus", help="pin every run to these cores instead (for collectors "
                                      "with helper threads, which need spare cores)")
    p.add_argument("--quick", action="store_true")
    p.add_argument("--out", help="append JSON lines here (default: stdout)")
    args = p.parse_args()

    env = dict(os.environ)
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v
    tiers = set(args.tier or ("pathology", "workloads", "threads"))
    out = open(args.out, "a") if args.out else sys.stdout
    for rep in range(1, args.repeat + 1):
        for tier, label, argv, threads in entries(args.quick):
            if tier not in tiers or (args.only and label not in args.only):
                continue
            cmd = ["taskset", "-c", cpus_for(threads, args), args.python, *argv]
            t0 = time.perf_counter()
            cp = subprocess.run(cmd, cwd=HERE, env=env, capture_output=True, text=True)
            rec = {"tier": tier, "label": label, "run": rep, "python": args.python,
                   "env": args.env, "rc": cp.returncode,
                   "process_s": round(time.perf_counter() - t0, 2)}
            if cp.returncode == 0:
                rec.update(json.loads(cp.stdout.strip().splitlines()[-1]))
            else:
                rec["error"] = cp.stderr[-1500:]
            print(json.dumps(rec), file=out, flush=True)
            print(f"{tier:9s} {label:24s} rc={cp.returncode} {rec['process_s']:6.1f}s",
                  file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
