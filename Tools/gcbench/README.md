# GC benchmarks

Benchmarks for CPython's cyclic garbage collector, for both the default
(GIL) and the free-threaded build. Most need nothing beyond the interpreter
under test and a C compiler (for the extension module used by some
workloads); they work with uninstalled build trees. The real-application
tier also needs pyperformance and a Sphinx installation.

## What they are good for

Most of these are **stress tests**, not realistic workloads: GC takes 7-90%
of their time, against a median of about 0.5% across pyperformance, and
almost all their garbage is cyclic, while real programs free most garbage
by reference counting. They are good at reproducing and explaining
specific collector problems; they exaggerate the effect of a GC change on
real programs. Use three tiers:

1. **Pathology tests** (`suite.py --tier pathology`): one known problem
   each; read their primary result (below).
2. **Steady-state workloads** (`suite.py --tier workloads` and `threads`):
   `web_service` has realistic proportions (a large mixed heap, mostly
   acyclic garbage, an LRU cache with turnover, sessions living many
   requests); run them for 30-60 s, or they may never reach a full
   collection.
3. **Real applications** (`realapps.py`): the GC-relevant pyperformance
   benchmarks and a Sphinx build of CPython's documentation, with the GC
   statistics hook.

## Files

| file | what |
|---|---|
| `suite.py` | runs the recommended set (pathology, workloads, threads), pinned to cores, one JSON line per run |
| `realapps.py` | real-application tier: pyperformance subset and the docs build with GC statistics |
| `hook/sitecustomize.py` | per-process GC statistics, used by `realapps.py` (active with `GCBENCH_STATS_DIR`) |
| `gcstats.py` | collection statistics shared by the benchmarks: young/full split, pause percentiles |
| `gen_frag.py` | page occupancy: `sparse` (scattered survivors), `churn` (a table with turnover) |
| `bigcollect.py` | `collect` (one large collection: time, RSS returned) and `container` (a large container built while young) |
| `revert_cases.py` | `migrations`, `doctree`, `deque`: the workloads behind the incremental-GC reverts |
| `workloads.py` | steady-state workloads run by a common multi-threaded harness |
| `gcbench.py` | synthetic micro-benchmarks: `collect`, `traverse`, `pause`, `release` |
| `scaling.py` | heap size, run time, stack depth and threads varied one at a time |
| `_gcbench_ext.c`, `buildext.py` | extension module with GC and non-GC types used by some workloads |
| `compare.py` | compares `suite.py` results (and `realapps.py` output) of two or more configurations: per benchmark its time or throughput, memory, longest collection and other primary results, with the change from the baseline |
| `perfgc.py` | analysis: `perf script` samples in the collector |
| `EXAMPLE.md`, `example/` | a complete example run, default build against free-threaded build, with its output and how to read it |

## Pathology tests and their primary results

| benchmark | problem it reproduces | primary result |
|---|---|---|
| `gen_frag.py sparse` | long-lived survivors scattered thinly over young pages | final RSS |
| `gen_frag.py churn [--acyclic F]` | medium-lived objects (a table with turnover) that die after promotion | time, full collections and their time |
| `bigcollect.py collect` | a 3M-object collection next to a live heap | final collection time; RSS before and after (is the memory returned?) |
| `bigcollect.py container` | a large container that stays young while it is filled | build time, young pauses |
| `revert_cases.py migrations` | Django `migrate`: classes living about one step (gh-148726) | peak RSS, longest pause (its wall time is GC-bound, not a real `migrate`'s) |
| `revert_cases.py doctree` | Sphinx/docutils: a heap growing with every document (gh-124717) | GC time per document against live nodes, per quarter; longest pause |
| `revert_cases.py deque` | Tim Peters' test: reclamation delay of garbage holding big payloads | backlog of uncollected garbage, peak RSS (timings say little) |
| `bigbuffers.py SIZE` | two-object cycles each holding a bytearray of SIZE bytes: memory from Python's allocator that the object count can't see (the suite runs 200 KB and 4 MB) | megabytes allocated per collection, peak RSS (the runs are too short to time) |
| `workloads.py ext_buffers_malloc`, `ext_buffers_pymem` | cycles holding large side buffers the count-based trigger can't see, taken from malloc() or from PyMem_Malloc() (`ext_buffers` mixes the two: two malloc() buffers and one PyMem_Malloc() buffer per step) | `side_peak_mb`, peak RSS (ops/s depends on the allocator as much as on the collector: read it next to `page_faults`, the memory handed back to the OS and taken again) |
| `workloads.py hidden_refs --ops N` | a slow leak through a cycle the GC can't see | throughput, leaked holders, peak RSS |
| `workloads.py c_globals` and `py_globals` | objects owned by a C static global (not reachable from known roots) | `c_globals` against `py_globals`: only meaningful as a pair |

## Steady-state workloads (`workloads.py`)

Every workload builds its long-lived data, then runs `--threads` worker
threads (default 4) for `--duration` seconds (default 30) or, with
`--ops N`, until N steps are done in total (the same work, and for leaking
workloads the same heap growth, for every configuration). A ticker thread
records every stall longer than 200 us unless `--no-ticker` is given.

| workload | real-world pattern | what makes it interesting for the GC |
|---|---|---|
| `web_service` | a web service: 2M-object mixed heap built gradually, LRU cache with turnover, sessions living 50 requests, request handling with mostly acyclic garbage and a few percent cyclic | realistic proportions: run for 60 s, and with 4-8 threads |
| `orm_session` | ORM unit of work: session with a `WeakValueDictionary` identity map, `__slots__` models with parent/children and session back-references | every request leaves cyclic garbage with weakrefs and weakref callbacks |
| `event_widgets` | GUI/event-emitter object tree: parent/children, listeners as bound methods, lambdas closing over `self`, `functools.partial` | many small cycles through bound methods, closures and cells |
| `xml_dom` | XML processing with `xml.dom.minidom` (parse, query, modify, serialize) | real library code; stdlib objects with parent/child links |
| `async_server` | asyncio request handling: `gather`, async generators closed early, exceptions kept in a local | exception/traceback/frame cycles, objects with finalizers |
| `json_churn` | `json` round trips next to a large configuration | acyclic garbage only: a control where automatic collections should not happen |
| `ext_buffers_malloc`, `ext_buffers_pymem`, `hidden_refs`, `c_globals`, `py_globals` | see the pathology tests | |

`orm_session`, `event_widgets`, `xml_dom` and `async_server` are **one
family**: a static long-lived heap plus short-lived, almost entirely cyclic
garbage. They differ in object types, not in what they ask of the
collector; treat them as one piece of evidence, not four.

Options that make them more realistic:

* `--acyclic F` adds request-like acyclic garbage to every step so that a
  fraction F (for example 0.9-0.99) of the GC objects a step leaves behind
  is acyclic, as in real programs. The harness measures a step's cyclic
  garbage before the run (`cyclic_objs_per_step`).
* `--medium N` keeps a small cyclic object graph per step for N steps: a
  medium-lived component (sessions, requests in flight) whose objects
  survive several young collections and then die.

`workloads.py` reports, per workload, its primary results first, then
throughput, the share of time spent in collections, collections split into
young and full with the median, p99 and longest pause, stalls (with the
ticker), RSS after setup and at its peak (and once a second in
`rss_series_mb`), and workload-specific counters.

## Collections: young and full

`gcstats.py` splits collections into young and full ones: in the default
build, generation 2 collections are full; in the free-threaded build every
collection is full unless generational collection is enabled
(`PYTHON_GC_GENERATIONAL=1`), which reports young collections as generation
0 and full ones as generation 2.

## Running

```sh
PY=path/to/build/python
$PY suite.py --python $PY --env PYTHON_GC_GENERATIONAL=1 --out gen.jsonl
$PY suite.py --python $PY --out nongen.jsonl
$PY suite.py --python $PY --quick              # smoke test only
$PY compare.py nongen.jsonl gen.jsonl --names nongen,gen
$PY workloads.py run web_service --threads 8 --duration 60
$PY realapps.py sphinx --venv DOCS_VENV --doc path/to/cpython/Doc --out out/sphinx
$PY realapps.py pyperf --python $PY --pyperformance "path/to/pyperformance" --out out/pyperf
```

`suite.py` pins single-threaded runs to two cores (`--cpus`, default 2,3)
and multi-threaded runs to one core per thread (`--thread-cpus`, default
0-7); pick performance cores on hybrid CPUs, and run one suite at a time on
an idle machine. For the docs build, create a virtual environment of the
interpreter under test with `Doc/requirements.txt` installed; on the
free-threaded build pass `--env PYTHON_GIL=0`, or MarkupSafe's speedups
module re-enables the GIL.

## Reporting results

* Give GC-time changes next to throughput changes, and the share of time
  spent in GC: a 10% faster collector saves 10% of the GC share, which is
  2-8% of these stress tests' time but typically 0.05-1.5% of a real
  program's (`realapps.py` reports the share for real programs).
* Lead with each benchmark's primary result; don't read the wall time of
  GC-bound pathology tests as representative.
* Compare several runs: most results vary by 2% or less, the longest pause
  much more (one outlier sets it), and multi-threaded runs more than
  single-threaded ones. In the default build, multi-threaded workloads are
  dominated by GIL contention, and stalls include GIL waits.
