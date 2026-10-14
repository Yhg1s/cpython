"""Scaling runs: extend one dimension of a benchmark at a time.

Runs every experiment below for every configuration (interpreter plus
environment) and writes one JSON line per run to --out; `--summary FILE`
prints tables from an existing results file.

    python scaling.py --config NAME PYTHON [K=V ...] [--config ...]
                      --out results.jsonl [--only EXPERIMENT ...]
    python scaling.py --summary results.jsonl

Experiments (one parameter varied, the others fixed):

  heap_traverse    full collection (gc.collect()) over N live objects
  heap_pause       pause benchmark, 4 threads, N live objects
  runtime_pause    pause benchmark, 4 threads, 1M live objects, N seconds
  runtime_leak     hidden_refs workload (leaks 1%), 4 threads, N seconds
  heap_doctree     revert_cases doctree with N documents
  heap_migrations  revert_cases migrations with N loaded migrations
  stack_pause      pause benchmark, 4 threads, 1M live, N frames deep
  stack_widgets    event_widgets workload, 4 threads, N frames deep
  threads_widgets  event_widgets workload, N threads
  threads_orm      orm_session workload, N threads
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# name: (python, {environment}), from --config
CONFIGS = {}

PAUSE = ["gcbench.py", "--json", "pause", "--threads", "4",
         "--sample-every", "0"]

EXPERIMENTS = {
    "heap_traverse": ([500_000, 1_000_000, 2_000_000, 4_000_000, 8_000_000],
                      lambda v: ["gcbench.py", "--json", "traverse",
                                 "--live", str(v), "--repeat", "3"]),
    "heap_pause": ([500_000, 1_000_000, 2_000_000, 4_000_000, 8_000_000],
                   lambda v: PAUSE + ["--live", str(v), "--duration", "5"]),
    "runtime_pause": ([5, 10, 20, 40],
                      lambda v: PAUSE + ["--live", "1000000", "--duration", str(v)]),
    "runtime_leak": ([5, 10, 20, 40],
                     lambda v: ["workloads.py", "run", "hidden_refs", "--json",
                                "--threads", "4", "--duration", str(v)]),
    "heap_doctree": ([1200, 2400, 4800, 9600],
                     lambda v: ["revert_cases.py", "doctree", "--docs", str(v)]),
    "heap_migrations": ([60_000, 120_000, 240_000, 480_000],
                        lambda v: ["revert_cases.py", "migrations", "--steps", "150",
                                   "--history", str(v)]),
    "stack_pause": ([0, 100, 1000, 10000],
                    lambda v: PAUSE + ["--live", "1000000", "--duration", "5",
                                       "--stack-depth", str(v)]),
    "stack_widgets": ([0, 100, 1000, 10000],
                      lambda v: ["workloads.py", "run", "event_widgets", "--json",
                                 "--threads", "4", "--duration", "5",
                                 "--stack-depth", str(v)]),
    "threads_widgets": ([1, 2, 4, 8, 16],
                        lambda v: ["workloads.py", "run", "event_widgets", "--json",
                                   "--threads", str(v), "--duration", "5"]),
    "threads_orm": ([1, 2, 4, 8, 16],
                    lambda v: ["workloads.py", "run", "orm_session", "--json",
                               "--threads", str(v), "--duration", "5"]),
}


def run_one(config, experiment, value, timeout):
    python, env = CONFIGS[config]
    cmd = [python] + EXPERIMENTS[experiment][1](value)
    full_env = dict(os.environ, **env)
    try:
        out = subprocess.run(cmd, cwd=HERE, env=full_env, capture_output=True,
                             text=True, timeout=timeout)
        line = out.stdout.strip().splitlines()[-1]
        result = json.loads(line)
    except Exception as e:
        result = {"error": repr(e)[:300]}
    result.update(config=config, experiment=experiment, value=value)
    return result


# (metric, label, format) per experiment
METRICS = {
    "heap_traverse": [("median_ms", "pause ms", "{:.0f}")],
    "heap_pause": [("cycles_per_s", "Mcyc/s", "{:.2f}", 1e-6),
                   ("gap_ms_max", "stall ms", "{:.0f}"),
                   ("peak_rss_mb", "peak MB", "{:.0f}")],
    "runtime_pause": [("cycles_per_s", "Mcyc/s", "{:.2f}", 1e-6),
                      ("gap_ms_max", "stall ms", "{:.0f}"),
                      ("peak_rss_mb", "peak MB", "{:.0f}")],
    "runtime_leak": [("ops_per_s", "Mops/s", "{:.2f}", 1e-6),
                     ("stall_ms_max", "stall ms", "{:.0f}"),
                     ("rss_peak_mb", "peak MB", "{:.0f}")],
    "heap_doctree": [("wall_s", "time s", "{:.2f}"),
                     ("gc_max_ms", "worst coll ms", "{:.0f}"),
                     ("peak_rss_mb", "peak MB", "{:.0f}")],
    "heap_migrations": [("wall_s", "time s", "{:.2f}"),
                        ("gc_max_ms", "worst coll ms", "{:.0f}"),
                        ("peak_rss_mb", "peak MB", "{:.0f}")],
    "stack_pause": [("cycles_per_s", "Mcyc/s", "{:.2f}", 1e-6),
                    ("gap_ms_max", "stall ms", "{:.0f}"),
                    ("peak_rss_mb", "peak MB", "{:.0f}")],
    "threads_widgets": [("ops_per_s", "kops/s", "{:.1f}", 1e-3),
                        ("stall_ms_max", "stall ms", "{:.0f}"),
                        ("rss_peak_mb", "peak MB", "{:.0f}")],
    "threads_orm": [("ops_per_s", "kops/s", "{:.1f}", 1e-3),
                    ("stall_ms_max", "stall ms", "{:.0f}"),
                    ("rss_peak_mb", "peak MB", "{:.0f}")],
    "stack_widgets": [("ops_per_s", "kops/s", "{:.1f}", 1e-3),
                      ("stall_ms_max", "stall ms", "{:.0f}"),
                      ("rss_peak_mb", "peak MB", "{:.0f}")],
}


def summarize(path):
    rows = [json.loads(l) for l in open(path)]
    for exp, (values, _) in EXPERIMENTS.items():
        rs = [r for r in rows if r["experiment"] == exp]
        if not rs:
            continue
        print(f"\n### {exp}\n")
        for m in METRICS[exp]:
            key, label, fmt = m[0], m[1], m[2]
            scale = m[3] if len(m) > 3 else 1
            print(f"{label}:\n")
            print("| config | " + " | ".join(f"{v:,}" for v in values) + " |")
            print("|---" * (len(values) + 1) + "|")
            for cfg in dict.fromkeys(x["config"] for x in rows):
                if not any(x["config"] == cfg for x in rs):
                    continue
                cells = []
                for v in values:
                    r = [x for x in rs if x["config"] == cfg and x["value"] == v]
                    ok = sorted(x[key] for x in r if "error" not in x and key in x)
                    if not ok:
                        cells.append("err" if r else "")
                    else:
                        # median of the runs of this point
                        cells.append(fmt.format(ok[len(ok) // 2] * scale))
                print(f"| {cfg} | " + " | ".join(cells) + " |")
            print()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out")
    p.add_argument("--summary")
    p.add_argument("--only", nargs="*")
    p.add_argument("--config", nargs="+", action="append", default=[],
                   metavar="NAME PYTHON [K=V]",
                   help="a configuration: a name, an interpreter and environment variables")
    p.add_argument("--configs", nargs="*", help="run only these of the configurations")
    p.add_argument("--timeout", type=int, default=900)
    args = p.parse_args()
    if args.summary:
        summarize(args.summary)
        return
    for name, python, *env in args.config:
        CONFIGS[name] = (python, dict(kv.split("=", 1) for kv in env))
    if not CONFIGS:
        p.error("give at least one --config")
    exps = args.only or list(EXPERIMENTS)
    cfgs = args.configs or list(CONFIGS)
    with open(args.out, "a") as f:
        for exp in exps:
            for value in EXPERIMENTS[exp][0]:
                for cfg in cfgs:
                    r = run_one(cfg, exp, value, args.timeout)
                    f.write(json.dumps(r) + "\n")
                    f.flush()
                    status = r.get("error", "ok")
                    print(f"{exp} {value} {cfg}: {status}", file=sys.stderr,
                          flush=True)


if __name__ == "__main__":
    main()
