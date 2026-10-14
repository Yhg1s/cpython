# Example: the default build against the free-threaded build

A complete run of these benchmarks against two unmodified builds of CPython
main, the default (GIL) build and the free-threaded build, with the output
they printed and how to read it. The raw results are in `example/`, so
every table below can be regenerated with `compare.py`.

## What was compared

* CPython main at c1af94ae348, built twice from the same tree: `configure`
  (default build) and `configure --disable-gil` (free-threaded build);
  release builds (no PGO or LTO), GCC 16.2.
* An Intel Core Ultra 7 265K (8 performance and 12 efficiency cores, 30 MiB
  L3), 62 GB of RAM, Linux 6.12, otherwise idle.
* `suite.py`'s default pinning: single-threaded benchmarks on performance
  cores 2-3, multi-threaded ones on performance cores 0-7.

Your numbers will differ with the machine; the relative differences
between the two builds are what carries over, and even those depend on the
core count for the threads tier.

## Commands

```sh
GIL=path/to/default-build/python
FT=path/to/free-threaded-build/python

# The suite, twice, alternating builds (each run appends one round to its file).
$GIL suite.py --python $GIL --out example/gil.jsonl
$FT suite.py --python $FT --out example/ft.jsonl
$GIL suite.py --python $GIL --out example/gil.jsonl
$FT suite.py --python $FT --out example/ft.jsonl

# Real applications: a docs build that is thrown away (it compiles .pyc
# files), then two measured ones; pyperformance once per build.
taskset -c 2,3 $GIL realapps.py sphinx --python $GIL --site-packages SITE \
    --doc path/to/cpython/Doc --out example/gil-sphinx1          # also ...warmup, ...2
taskset -c 2,3 $FT realapps.py sphinx --python $FT --site-packages SITE \
    --doc path/to/cpython/Doc --out example/ft-sphinx1 --env PYTHON_GIL=0
taskset -c 2,3 $GIL realapps.py pyperf --python $GIL --pyperformance PYPERFORMANCE \
    --out example/gil-pyperf
taskset -c 2,3 $FT realapps.py pyperf --python $FT --pyperformance PYPERFORMANCE \
    --out example/ft-pyperf

# The comparison.
python3 compare.py example/gil.jsonl example/ft.jsonl --names gil,ft \
    --sphinx example/gil-sphinx1,example/ft-sphinx1 \
    --sphinx example/gil-sphinx2,example/ft-sphinx2 \
    --pyperf example/gil-pyperf,example/ft-pyperf
```

`SITE` is the `site-packages` of a virtual environment with
`Doc/requirements.txt` installed (one of a free-threaded interpreter was used
for both builds here; the default build falls back to pure-Python versions of
the compiled modules). `PYTHON_GIL=0` keeps MarkupSafe's speedups module from
re-enabling the GIL in the free-threaded build. The suite takes about 13
minutes per build and round; pyperformance about 20 minutes per build.

Alternating the builds spreads slow drift (thermal, background activity)
over both. Two rounds are the minimum to see how noisy a result is; use
three or more when the differences you care about are a few percent.

## What the suite prints

`suite.py` prints one line per benchmark (exit status and wall time) and
writes the results to `--out`; the first round on the default build:

```
pathology gen_frag sparse          rc=0    0.6s
pathology gen_frag churn           rc=0    8.4s
pathology gen_frag churn acyclic   rc=0    4.6s
pathology bigcollect               rc=0    1.1s
pathology container                rc=0    1.2s
pathology doctree                  rc=0   12.1s
pathology migrations               rc=0   10.5s
pathology deque                    rc=0    1.2s
pathology bigbuffers 200KB         rc=0    0.2s
pathology bigbuffers 4MB           rc=0    0.6s
pathology ext_buffers_malloc       rc=0   30.2s
pathology ext_buffers_pymem        rc=0   30.2s
pathology hidden_refs              rc=0   11.8s
pathology c_globals                rc=0   30.4s
pathology py_globals               rc=0   30.4s
pathology event_widgets runaway    rc=0    4.6s
workloads web_service              rc=0   61.5s
workloads orm_session mixed        rc=0   60.2s
workloads async_server             rc=0   30.1s
workloads json_churn               rc=0   30.3s
threads   web_service 4t           rc=0   61.5s
threads   web_service 8t           rc=0   61.9s
threads   orm_session mixed 4t     rc=0   60.4s
```

## One benchmark on its own

Every benchmark can be run directly; `workloads.py` prints its primary
results first, then the rest. web_service with four threads on four cores,
30 seconds, default build:

```
web_service    ops_per_s=46103.7  gc_ms_p99=82.9  gc_ms_max=343.1  rss_peak_mb=385.3  |       46104 ops/s  gc 41.9% in 1742 x (1729 young, 13 full; med   0.40, p99  82.91, max  343.13 ms)  stall max   402.5 ms, total  25199 ms  rss   288 -> peak   385 MB  page_faults=24935  cache_hit_rate=0.2  live_models=300000
```

Free-threaded build:

```
web_service    ops_per_s=140603.0  gc_ms_p99=357.1  gc_ms_max=357.1  rss_peak_mb=572.5  |      140603 ops/s  gc 25.1% in   30 x (0 young, 30 full; med 251.04, p99 357.12, max  357.12 ms)  stall max   129.1 ms, total   4944 ms  rss   323 -> peak   573 MB  page_faults=235686  cache_hit_rate=0.3  live_models=300000
```

* `ops/s`: throughput over the measured part (setup excluded).
* `gc 41.9% in 1742 x (1729 young, 13 full; ...)`: the share of the run
  spent in collections, the number of collections split into young and full
  ones, and the median, p99 and longest collection, measured from the start
  to the end of each collection on the thread that ran it.
* `stall max ..., total ...`: what the application sees. A ticker thread
  records every gap longer than 200 us in its own progress. In the
  free-threaded build a collection's freeing phase runs with the other
  threads going again, so the longest stall (129 ms) is shorter than the
  longest collection (357 ms); in the default build, stalls also include
  waiting for the GIL (25 s in total over four threads here), so they
  measure contention as much as the collector.
* `rss 288 -> peak 385 MB`: resident memory after setup and at its peak.
* `page_faults`: minor page faults in the measured part. Memory handed back
  to the operating system and taken again shows up here; 236k faults in the
  free-threaded run against 25k is a hint that its allocator returns and
  re-faults memory.
* The rest (`cache_hit_rate`, `live_models`) are the workload's own counters.

The free-threaded build runs the four threads in parallel, which is why it
does three times the work; that is free threading, not the collector.

## The suite compared

`compare.py` prints, for each benchmark, its time or throughput, its memory
and its longest collection, then its other primary results (the ones
README.md says to read): the mean of the two rounds with their range, and
the change of the free-threaded build from the default build in percent:

Results per benchmark (mean, range over runs; change from gil; lower is better unless the result says otherwise):

| benchmark | result | gil | ft | change |
|---|---|---|---|---|
| gen_frag sparse | run time | 0.56 s (0.55-0.56) | 1.22 s (1.20-1.23) | +119% |
|  | final RSS | 45 MB | 66 MB | +47% |
|  | longest collection | 24.2 ms (21.8-26.6) | 16.1 ms (15.5-16.6) | -34% |
| gen_frag churn | run time | 8.00 s (7.91-8.08) | 4.14 s (4.11-4.18) | -48% |
|  | final RSS | 93 MB | 100 MB | +7.5% |
|  | longest collection | 319 ms (314-324) | 53.3 ms (50.5-56.1) | -83% |
|  | full collections | 25 | 72 | +188% |
|  | time in full collections | 6.32 s (6.23-6.40) | 2.70 s (2.68-2.73) | -57% |
| gen_frag churn acyclic | run time | 4.44 s (4.40-4.48) | 2.26 s (2.25-2.27) | -49% |
|  | final RSS | 91 MB | 100 MB | +9.9% |
|  | longest collection | 332 ms (331-333) | 47.6 ms (47.0-48.1) | -86% |
|  | full collections | 6 | 24 | +300% |
|  | time in full collections | 1.01 s (1.01-1.02) | 0.43 s (0.42-0.44) | -58% |
| bigcollect | final collection | 344 ms (342-345) | 205 ms (205-206) | -40% |
|  | RSS after collection | 262 MB (262-263) | 292 MB | +11% |
|  | RSS before collection | 286 MB (285-286) | 316 MB | +11% |
| container | build time | 0.77 s (0.77-0.78) | 0.48 s (0.47-0.49) | -37% |
|  | RSS | 247 MB | 290 MB | +17% |
|  | longest collection | 140 ms (138-142) | 74.0 ms (67.2-80.8) | -47% |
| doctree | run time | 8.12 s (8.11-8.13) | 4.70 s (4.70-4.71) | -42% |
|  | peak RSS | 1,168 MB (1,167-1,168) | 1,229 MB (1,229-1,229) | +5.2% |
|  | longest collection | 1,058 ms (1,057-1,059) | 347 ms (347-348) | -67% |
| migrations | run time | 10.2 s (10.2-10.2) | 5.57 s (5.55-5.58) | -46% |
|  | peak RSS | 280 MB | 320 MB (320-320) | +14% |
|  | longest collection | 242 ms (238-245) | 107 ms (106-109) | -56% |
|  | largest garbage backlog | 11 objects | 11 objects | 0% |
| deque | run time | 1.14 s (1.13-1.14) | 1.61 s (1.60-1.61) | +41% |
|  | peak RSS | 242 MB | 113 MB (112-113) | -54% |
|  | longest collection | 6.16 ms (5.82-6.50) | 1.86 ms (1.83-1.89) | -70% |
|  | largest garbage backlog | 23,010 objects (23,009-23,012) | 5,495 objects (5,491-5,499) | -76% |
| bigbuffers 200KB | run time | 0.19 s (0.19-0.19) | 0.16 s (0.16-0.16) | -15% |
|  | peak RSS | 207 MB (207-207) | 556 MB (556-556) | +168% |
|  | longest collection | 0.93 ms (0.93-0.93) | 1.11 ms (1.10-1.12) | +19% |
|  | allocated between collections | 182 MB | 400 MB | +120% |
| bigbuffers 4MB | run time | 0.51 s (0.51-0.51) | 0.26 s (0.26-0.26) | -49% |
|  | peak RSS | 1,898 MB (1,898-1,898) | 2,124 MB (2,124-2,124) | +12% |
|  | longest collection | 1.60 ms (1.49-1.70) | none | - |
|  | allocated between collections | 2,000 MB | 2,000 MB | 0% |
| ext_buffers_malloc | throughput (higher is better) | 597,779 ops/s (597,133-598,424) | 160,498 ops/s (150,128-170,867) | -73% |
|  | side-buffer peak | 127 MB (126-129) | 351 MB (323-380) | +176% |
|  | longest collection | 0.93 ms (0.81-1.04) | 21.4 ms (19.7-23.1) | +2213% |
|  | peak RSS | 193 MB (192-195) | 442 MB (412-473) | +129% |
|  | GC share | 19.3% (19.2-19.3) | 20.3% (19.5-21.1) | +5.3% |
| ext_buffers_pymem | throughput (higher is better) | 594,385 ops/s (592,992-595,778) | 258,293 ops/s (257,662-258,924) | -57% |
|  | side-buffer peak | 127 MB (127-128) | 323 MB (322-324) | +153% |
|  | longest collection | 0.74 ms (0.39-1.10) | 14.1 ms (13.1-15.1) | +1799% |
|  | peak RSS | 194 MB (192-195) | 469 MB (469-470) | +143% |
|  | GC share | 19.3% (19.3-19.4) | 24.5% (24.3-24.7) | +27% |
| hidden_refs | throughput (higher is better) | 1,814,830 ops/s (1,802,473-1,827,186) | 401,540 ops/s (400,122-402,957) | -78% |
|  | peak RSS | 308 MB (308-309) | 445 MB (445-445) | +44% |
|  | longest collection | 111 ms (97.4-124) | 151 ms (151-152) | +36% |
|  | leaked holders | 200,003 | 200,003 | 0% |
|  | GC share | 37.1% (36.8-37.4) | 72.8% (72.7-72.8) | +96% |
| c_globals | throughput (higher is better) | 288,505 ops/s (287,504-289,506) | 244,721 ops/s (239,431-250,011) | -15% |
|  | peak RSS | 127 MB (127-128) | 200 MB (200-200) | +57% |
|  | longest collection | 0.55 ms (0.54-0.57) | 50.7 ms (50.6-50.8) | +9038% |
|  | p99 collection | 0.37 ms (0.37-0.37) | 48.4 ms (47.3-49.5) | +12886% |
|  | GC share | 7.28% (7.26-7.30) | 18.5% (18.0-19.0) | +154% |
| py_globals | throughput (higher is better) | 285,087 ops/s (283,825-286,349) | 244,817 ops/s (243,837-245,797) | -14% |
|  | peak RSS | 127 MB (127-127) | 194 MB (194-194) | +53% |
|  | longest collection | 0.57 ms (0.53-0.61) | 37.7 ms (37.7-37.8) | +6484% |
|  | p99 collection | 0.38 ms (0.38-0.38) | 37.5 ms (37.5-37.6) | +9869% |
|  | GC share | 7.27% (7.27-7.27) | 14.6% (14.6-14.6) | +101% |
| event_widgets runaway | throughput (higher is better) | 23,436 ops/s (23,325-23,547) | 41,989 ops/s (40,974-43,003) | +79% |
|  | peak RSS | 141 MB (138-144) | 3,514 MB (3,463-3,565) | +2386% |
|  | longest collection | 132 ms (116-147) | 2,173 ms (2,163-2,183) | +1552% |
|  | p99 collection | 102 ms (101-103) | 2,173 ms (2,163-2,183) | +2031% |
|  | GC share | 98.5% (98.3-98.7) | 95.9% (95.6-96.2) | -2.6% |
| web_service | throughput (higher is better) | 54,433 ops/s (54,363-54,502) | 56,248 ops/s (56,081-56,414) | +3.3% |
|  | peak RSS | 374 MB (373-374) | 516 MB (516-516) | +38% |
|  | longest collection | 279 ms (279-280) | 227 ms (226-227) | -19% |
|  | p99 collection | 9.89 ms (9.86-9.92) | 227 ms (226-227) | +2191% |
|  | GC share | 25.4% (25.4-25.4) | 9.54% (9.53-9.56) | -62% |
| orm_session mixed | throughput (higher is better) | 2,581 ops/s (2,576-2,586) | 1,822 ops/s (1,818-1,825) | -29% |
|  | peak RSS | 87.3 MB (87.2-87.5) | 147 MB (146-147) | +68% |
|  | longest collection | 7.81 ms (7.61-8.02) | 20.6 ms (17.4-23.9) | +164% |
|  | p99 collection | 0.62 ms (0.62-0.62) | 17.0 ms (16.7-17.2) | +2632% |
|  | GC share | 11.4% (11.4-11.4) | 22.6% (22.4-22.7) | +98% |
| async_server | throughput (higher is better) | 9,594 ops/s (9,528-9,661) | 7,750 ops/s (7,749-7,750) | -19% |
|  | peak RSS | 44.2 MB (44.1-44.2) | 77.5 MB (77.5-77.5) | +76% |
|  | longest collection | 5.01 ms (4.76-5.26) | 5.87 ms (5.74-6.00) | +17% |
|  | p99 collection | 0.12 ms | 5.71 ms (5.58-5.83) | +4863% |
|  | GC share | 1.61% (1.59-1.63) | 5.23% (5.14-5.31) | +224% |
| json_churn | throughput (higher is better) | 70,834 ops/s (70,567-71,102) | 59,383 ops/s (59,041-59,726) | -16% |
|  | peak RSS | 84.6 MB (84.5-84.7) | 120 MB (120-120) | +41% |
|  | longest collection | none | none | - |
|  | p99 collection | none | none | - |
|  | GC share | 0% | 0% | - |
| web_service 4t | throughput (higher is better) | 54,292 ops/s (54,175-54,410) | 174,782 ops/s (174,600-174,965) | +222% |
|  | peak RSS | 385 MB (384-386) | 553 MB (553-554) | +44% |
|  | longest collection | 365 ms (355-375) | 273 ms (271-275) | -25% |
|  | p99 collection | 63.8 ms (61.8-65.8) | 269 ms (269-270) | +322% |
|  | GC share | 43.7% (43.3-44.1) | 31.2% (31.2-31.2) | -29% |
| web_service 8t | throughput (higher is better) | 56,265 ops/s (56,161-56,370) | 278,211 ops/s (278,028-278,395) | +394% |
|  | peak RSS | 404 MB (402-406) | 630 MB (629-631) | +56% |
|  | longest collection | 544 ms (526-562) | 340 ms (337-344) | -37% |
|  | p99 collection | 180 ms (179-181) | 335 ms (335-335) | +86% |
|  | GC share | 62.6% (62.1-63.1) | 48.9% (48.9-49.0) | -22% |
| orm_session mixed 4t | throughput (higher is better) | 2,427 ops/s (2,426-2,429) | 4,100 ops/s (4,084-4,117) | +69% |
|  | peak RSS | 96.8 MB (96.6-97.1) | 169 MB (168-170) | +74% |
|  | longest collection | 82.0 ms (79.8-84.3) | 40.6 ms (31.3-50.0) | -50% |
|  | p99 collection | 23.2 ms (22.9-23.6) | 29.7 ms (29.0-30.5) | +28% |
|  | GC share | 39.6% (38.9-40.3) | 61.6% (61.1-62.1) | +55% |

### How to read it

* **Lower is better unless the result says otherwise**: -48% on a run time
  means the free-threaded build needs about half the time, -73% on a
  throughput that it does about a quarter of the work.
* **Lead with each benchmark's primary results** (README.md lists them).
  The run time of a GC-bound pathology test is mostly the collector and the
  allocator at work: gen_frag sparse takes 119% longer on the free-threaded
  build, but what it tests is the memory left behind and the longest
  collection.
* **Compare the difference with the ranges.** When the two ranges are far
  apart (doctree's longest collection, 1,057-1,059 ms against 347-348 ms)
  the difference is real; when they overlap, it isn't established. Most
  results here repeat within 1-3%; the longest collection much less well
  (one outlier sets it: gen_frag sparse 21.8-26.6 ms), and
  ext_buffers_malloc's throughput on the free-threaded build varies by 13%
  between rounds (its speed depends on how its glibc heap is laid out; read
  its `page_faults`).
* **Separate the interpreter from the collector.** json_churn never
  collects in either build: its 16% lower throughput is the free-threaded
  interpreter's general single-threaded overhead on this machine. A
  benchmark 16% slower on the free-threaded build is therefore about even
  as far as the collector goes; orm_session (-29%) and async_server (-19%)
  do worse than that, web_service (+3.3%) better.
* **GC share is not run time.** web_service spends 9.5% of its time
  collecting on the free-threaded build against 25.4% on the default build,
  yet runs only 3.3% faster; ext_buffers_malloc spends about the same share
  in both and has 73% less throughput. Late collections are paid outside
  the collector: in the allocator, in page faults and in cache misses.
* **Read every benchmark.** `compare.py` doesn't average the suite into one
  number: the benchmarks measure different things, and an average would mix
  the free-threaded build's parallelism (+222% throughput on web_service
  with four threads) with its interpreter overhead and with opposite
  results on the pathology tests.

What this comparison shows about the two collectors:

* **The default build has a young generation, the free-threaded build
  doesn't.** Every free-threaded collection is a full one
  (`full collections`), so its typical pauses grow with the heap: the p99
  collection is 99-130 times longer on c_globals and py_globals (0.37 ms
  against 37-48 ms), 23 times on web_service, 27 times on orm_session.
* **Where the default build's full collections are the problem, the
  free-threaded build is better.** doctree (a heap that grows with every
  document) has a longest collection of 347 ms instead of 1,058 ms (-67%)
  and runs in 42% less time; migrations' longest collection is -56%,
  container's build time -37%, bigcollect's final collection -40%, and
  gen_frag churn's time in full collections -57%: there the free-threaded
  build runs more full collections (72 against 25) but each costs far less
  (2.7 s in total against 6.3 s).
* **Memory is higher on the free-threaded build on every benchmark but
  one**, mostly by 5-76%, and by 150-180% where garbage holds large
  buffers: ext_buffers' side buffers peak 153% and 176% higher, and
  bigbuffers with 200 KB buffers peaks 168% higher, allocating 400 MB
  between collections instead of 182 MB. deque is the exception (54% less
  memory, a garbage backlog 76% smaller): the free-threaded build collects
  that garbage sooner.
* **event_widgets runaway shows a real problem.** Four threads create cyclic
  garbage faster than one thread frees it: the free-threaded build reaches
  3.5 GB in four seconds (+2386%, 25 times the default build) with
  collections of more than 2 s. Its higher throughput (+79%) is the
  parallel threads, not a better collector.
* c_globals and py_globals only mean something as a pair: their throughputs
  are within 1-2% of each other on both builds, so objects owned by a C
  global are no problem for either collector.

## Real applications

The docs build, as `realapps.py` prints it (default build, then
free-threaded build):

```
sphinx-build exited with 0 after 47 s; log in OUT/gil-sphinx1/sphinx.log
benchmark                    mode   procs GC share  colls  young  full  p99 ms  max ms  RSS MB
docs build                   gil        1   25.90%   7195   7178    17    1.26  3137.0     917
```

```
sphinx-build exited with 0 after 45 s; log in OUT/ft-sphinx1/sphinx.log
benchmark                    mode   procs GC share  colls  young  full  p99 ms  max ms  RSS MB
docs build                   ft         1   13.59%     54      0    54  547.87  1038.7    1093
```

pyperformance, the GC-relevant subset (`procs` is the number of worker
processes pyperformance started; the statistics are summed over them):

```
pyperformance exited with 0; log in OUT/gil-pyperf/pyperf.log
benchmark                    mode   procs GC share  colls  young  full  p99 ms  max ms  RSS MB
async_tree_io                gil       11   48.40%  14971  14861   110    2.56   106.9     142
async_tree_memoization       gil       11   39.38%   8646   8591    55    4.14    73.7      94
async_tree_none              gil       11   29.22%   8096   8041    55    0.77    29.7      89
django_template              gil       11    1.43%    451    451     0    1.12     1.2      49
docutils                     gil       11   13.46%  14597  14487   110    0.94    34.6      76
networkx_connected_components gil       11   11.71%   8118   8063    55    0.82   116.0     297
networkx_k_core              gil       11   11.19%  12441  12364    77    5.88   164.9     347
networkx_shortest_path       gil       11   11.56%   8118   8063    55    0.85   116.7     299
sphinx                       gil       11    8.76%   4026   4004    22    1.46    49.2     111
sqlalchemy_declarative       gil       11    1.44%    385    385     0    1.07     1.3      48
sqlalchemy_imperative        gil       11    2.70%    849    849     0    1.01     1.2      48
xdsl                         gil       11    7.60%   2204   2193    11    1.32    24.0      59
```

```
pyperformance exited with 0; log in OUT/ft-pyperf/pyperf.log
benchmark                    mode   procs GC share  colls  young  full  p99 ms  max ms  RSS MB
async_tree_io                ft        11   29.88%    341      0   341   30.55    31.6     165
async_tree_memoization       ft        11   30.79%    308      0   308   19.13    22.9     115
async_tree_none              ft        11   28.33%    297      0   297   16.94    17.3     108
django_template              ft        11    2.30%     99      0    99    3.72     3.7      70
docutils                     ft        11   21.81%   1683      0  1683   13.74    15.6      81
networkx_connected_components ft        11    6.83%    231      0   231   50.98    51.7     343
networkx_k_core              ft        11    3.98%    242      0   242   70.86    71.3     397
networkx_shortest_path       ft        11    6.75%    231      0   231   50.84    51.4     353
sphinx                       ft        11   14.50%    275      0   275   25.46    26.9     134
sqlalchemy_declarative       ft        11    2.33%     99      0    99    3.20     3.4      69
sqlalchemy_imperative        ft        11    6.44%    155      0   155    5.43     5.8      69
xdsl                         ft        11   10.94%    247      0   247    7.28     7.4      61
```

And compared:

Docs build (mean and range over runs; change from gil; lower is better):

| result | gil | ft | change |
|---|---|---|---|
| time | 45.5 s (45.4-45.5) | 43.9 s (43.8-44.0) | -3.4% |
| GC share | 25.9% (25.9-25.9) | 13.6% (13.6-13.7) | -47% |
| collections | 7,195 | 54 | -99% |
| longest collection | 3,147 ms (3,137-3,157) | 1,038 ms (1,037-1,039) | -67% |
| peak RSS | 917 MB | 1,093 MB | +19% |

pyperformance (median time; GC share and peak RSS from the statistics hook; change from gil, lower is better):

| benchmark | gil time | GC share | peak RSS | ft time | GC share | peak RSS | ft time change |
|---|---|---|---|---|---|---|---|
| async_tree_io | 422 ms | 48.4% | 142 MB | 354 ms | 29.9% | 165 MB | -16% |
| async_tree_memoization | 221 ms | 39.4% | 94 MB | 226 ms | 30.8% | 115 MB | +2.3% |
| async_tree_none | 157 ms | 29.2% | 89 MB | 176 ms | 28.3% | 108 MB | +13% |
| django_template | 15.7 ms | 1.4% | 49 MB | 18.0 ms | 2.3% | 70 MB | +15% |
| docutils | 1,105 ms | 13.5% | 76 MB | 1,385 ms | 21.8% | 81 MB | +25% |
| networkx_connected_components | 368 ms | 11.7% | 297 MB | 424 ms | 6.8% | 343 MB | +15% |
| networkx_k_core | 1,742 ms | 11.2% | 347 MB | 1,838 ms | 4.0% | 397 MB | +5.5% |
| networkx_shortest_path | 370 ms | 11.6% | 299 MB | 438 ms | 6.8% | 353 MB | +18% |
| sphinx | 446 ms | 8.8% | 111 MB | 522 ms | 14.5% | 134 MB | +17% |
| sqlalchemy_declarative | 56.1 ms | 1.4% | 48 MB | 63.6 ms | 2.3% | 69 MB | +13% |
| sqlalchemy_imperative | 7.79 ms | 2.7% | 48 MB | 9.16 ms | 6.4% | 69 MB | +18% |
| xdsl_constant_fold | 21.7 ms | 7.6% | 59 MB | 25.1 ms | 10.9% | 61 MB | +16% |

ft: time change from gil, geometric mean: +11%

* **The docs build is the closest thing to a real program here.** The
  free-threaded build is 3.4% faster (the two runs of each build are within
  0.2 s), spends half the share of its time collecting (13.6% against
  25.9%), and its longest collection is a third (1.0 s against 3.1 s,
  -67%): the default build's full collections over the growing document
  tree are what doctree in the pathology tier reproduces. It uses 19% more
  memory.
* **pyperformance is 11% slower on the free-threaded build overall**, in
  line with json_churn, which never collects and has 16% less throughput
  than on the default build. The benchmarks that spend a large share of
  their time collecting move away from that: async_tree_io is 16% faster
  (its GC share falls from 48% to 30%), docutils 25% slower (its GC share
  rises from 13% to 22%). Times are from one pyperformance run per build
  (`--fast`, `realapps.py`'s default), so differences of a few percent
  between single benchmarks are noise.
* Here too the free-threaded build runs fewer, longer collections: 341
  against 14,971 for async_tree_io, with a p99 of 31 ms against 2.6 ms.

## In short

To judge a collector change, run the suite and the real applications on a
build with and without the change, at least twice, and read in this order:

1. The real applications and the workloads tier: does anything realistic get
   slower, use more memory or pause longer?
2. The pathology tier, each benchmark's primary results: which known
   problem got better or worse, and by more than its range?
3. The other results of each benchmark (run time, GC share, collection
   counts) to explain what the primary results show.

When comparing two builds rather than two versions of a collector, as here,
keep json_churn in mind as the measure of everything that isn't the
collector.
