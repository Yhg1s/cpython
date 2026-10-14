"""Real-application tier: GC statistics of real programs.

The gcbench workloads are stress tests; their GC share is 10-100 times that
of typical programs. This tier measures a GC change on real application
code, with the share of time it spends collecting.

    python realapps.py pyperf --python PY --pyperformance CMD --out DIR
                              [--venvs-dir DIR] [--env K=V ...] [--bench B,...]
    python realapps.py sphinx (--venv VENV | --python PY --site-packages DIR)
                              --doc DIR --out DIR [--env K=V ...]
    python realapps.py summary DIR

  pyperf   Runs the GC-relevant pyperformance benchmarks (by default sphinx,
           docutils, three async_tree variants, the two sqlalchemy ones,
           django_template, three networkx ones and xdsl_constant_fold) with
           the GC statistics hook (hook/sitecustomize.py) in every worker
           process. --mode picks pyperformance's mode: fast (the default,
           --fast), default (pyperformance's default, more processes and
           values) or rigorous (--rigorous).
  sphinx   Builds CPython's documentation (DIR is a CPython Doc directory)
           with Sphinx, single-process, from scratch, with the hook: a large
           real run (a heap of millions of objects of mixed ages, built over
           minutes). VENV is a virtual environment of the interpreter under
           test with Doc/requirements.txt installed; or give the interpreter
           and a site-packages directory with those packages (one
           installation serves every build of the same Python version).
  summary  Per benchmark (or the docs build): processes, wall time, GC share
           of wall time, collections (young and full), longest and p99
           pause, peak RSS.

Report GC changes next to the GC share: a 10% faster collector saves 10% of
the GC share, which in typical programs is well under 1% of their time.
"""

import argparse
import collections
import glob
import json
import os
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOOK = os.path.join(HERE, "hook")
DEFAULT_BENCHES = ("sphinx,docutils,async_tree_none,async_tree_io,async_tree_memoization,"
                   "sqlalchemy_declarative,sqlalchemy_imperative,django_template,"
                   "networkx_connected_components,networkx_k_core,networkx_shortest_path,"
                   "xdsl_constant_fold")


def env_with(extra, stats_dir):
    env = dict(os.environ)
    for kv in extra:
        k, _, v = kv.partition("=")
        env[k] = v
    env["GCBENCH_STATS_DIR"] = stats_dir
    env["PYTHONPATH"] = HOOK + (os.pathsep + env["PYTHONPATH"] if env.get("PYTHONPATH") else "")
    return env


def cmd_pyperf(args):
    stats = os.path.join(args.out, "gcstats")
    os.makedirs(stats, exist_ok=True)
    env = env_with(args.env, stats)
    inherit = ",".join(["GCBENCH_STATS_DIR", "PYTHONPATH"] +
                       [kv.partition("=")[0] for kv in args.env])
    mode = {"fast": ["--fast"], "default": [], "rigorous": ["--rigorous"]}[args.mode]
    cmd = [*args.pyperformance.split(), "run", *mode, "--inherit-environ", inherit,
           "-p", args.python, "-b", args.bench, "-o", os.path.join(args.out, "pyperf.json")]
    if args.venvs_dir:
        cmd += ["--venvs-dir", args.venvs_dir]
    with open(os.path.join(args.out, "pyperf.log"), "w") as log:
        rc = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    print(f"pyperformance exited with {rc}; log in {args.out}/pyperf.log", file=sys.stderr)
    summary(args.out)


def cmd_sphinx(args):
    stats = os.path.join(args.out, "gcstats")
    os.makedirs(stats, exist_ok=True)
    build = os.path.join(args.out, "html")
    shutil.rmtree(build, ignore_errors=True)
    env = env_with(args.env, stats)
    if args.venv:
        py = os.path.join(args.venv, "bin", "python")
    else:
        py = args.python
        env["PYTHONPATH"] = env["PYTHONPATH"] + os.pathsep + args.site_packages
    cmd = [py, "-m", "sphinx", "-b", "html", "-E", "-q", "-j", "1", args.doc, build]
    t0 = time.perf_counter()
    with open(os.path.join(args.out, "sphinx.log"), "w") as log:
        rc = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    print(f"sphinx-build exited with {rc} after {time.perf_counter() - t0:.0f} s; "
          f"log in {args.out}/sphinx.log", file=sys.stderr)
    summary(args.out)


def bench_of(argv):
    """The pyperformance benchmark a worker runs, "docs build" for Sphinx,
    or None (pyperformance's own processes)."""
    if any(a.endswith(("sphinx", "sphinx/__main__.py")) or a == "sphinx" for a in argv):
        if "--worker" not in argv:
            return "docs build"
    if "--worker" not in argv:
        return None
    for a in argv:
        if a.endswith("run_benchmark.py"):
            name = os.path.basename(os.path.dirname(a))[3:]
            # A variant (async_tree "none", networkx "k_core") is a trailing
            # positional argument, not the value of an option.
            last, prev = argv[-1], argv[-2] if len(argv) > 1 else ""
            if (not last.startswith("-") and not prev.startswith("-")
                    and not last.replace(".", "").isdigit()):
                return f"{name}_{last}"
            return name
    return None


def is_full(mode, generation):
    return mode == "ft" or generation >= 2


def summary(out):
    per = collections.defaultdict(list)
    for f in glob.glob(os.path.join(out, "gcstats", "gcstats-*.jsonl")):
        for line in open(f):
            rec = json.loads(line)
            b = bench_of(rec["argv"])
            if b:
                per[b].append(rec)
    rows = []
    for b in sorted(per):
        recs = per[b]
        wall = sum(r["wall_s"] for r in recs)
        us = [c[0] for r in recs for c in r["colls"]]
        full = [c[0] for r in recs for c in r["colls"] if is_full(r["gc_mode"], c[1])]
        young = len(us) - len(full)
        us_sorted = sorted(us)
        rows.append({
            "bench": b, "gc_mode": recs[0]["gc_mode"], "processes": len(recs),
            "wall_s": round(wall, 2), "gc_share": round(sum(us) / 1e6 / wall, 4) if wall else 0,
            "collections": len(us), "young": young, "full": len(full),
            "pause_p99_ms": round(us_sorted[min(len(us) - 1, int(0.99 * (len(us) - 1)))] / 1e3, 2) if us else 0,
            "pause_max_ms": round(max(us) / 1e3, 2) if us else 0,
            "full_max_ms": round(max(full) / 1e3, 2) if full else 0,
            "maxrss_mb": round(statistics.median(r.get("maxrss_mb", 0) for r in recs)),
        })
    with open(os.path.join(out, "summary.jsonl"), "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
    print(f"{'benchmark':28s} {'mode':6s} {'procs':>5s} {'GC share':>8s} {'colls':>6s} "
          f"{'young':>6s} {'full':>5s} {'p99 ms':>7s} {'max ms':>7s} {'RSS MB':>7s}")
    for r in rows:
        print(f"{r['bench']:28s} {r['gc_mode']:6s} {r['processes']:5d} {r['gc_share'] * 100:7.2f}% "
              f"{r['collections']:6d} {r['young']:6d} {r['full']:5d} {r['pause_p99_ms']:7.2f} "
              f"{r['pause_max_ms']:7.1f} {r['maxrss_mb']:7d}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("pyperf")
    s.add_argument("--python", required=True)
    s.add_argument("--pyperformance", default="pyperformance",
                   help="command that runs pyperformance")
    s.add_argument("--venvs-dir")
    s.add_argument("--bench", default=DEFAULT_BENCHES)
    s.add_argument("--mode", choices=("fast", "default", "rigorous"), default="fast",
                   help="pyperformance's mode (default: fast)")
    s.add_argument("--env", action="append", default=[], metavar="K=V")
    s.add_argument("--out", required=True)
    s = sub.add_parser("sphinx")
    s.add_argument("--venv")
    s.add_argument("--python")
    s.add_argument("--site-packages")
    s.add_argument("--doc", required=True)
    s.add_argument("--env", action="append", default=[], metavar="K=V")
    s.add_argument("--out", required=True)
    s = sub.add_parser("summary")
    s.add_argument("out")
    args = p.parse_args()
    {"pyperf": cmd_pyperf, "sphinx": cmd_sphinx,
     "summary": lambda a: summary(a.out)}[args.cmd](args)


if __name__ == "__main__":
    main()
