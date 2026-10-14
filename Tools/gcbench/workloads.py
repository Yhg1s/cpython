"""Realistic GC workloads, run by a common multi-threaded harness.

Each workload models a usage pattern from real applications. The harness
runs N worker threads that repeatedly call the workload's step() for a fixed
time, next to a "ticker" thread that records every stall it sees (as in
gcbench.py pause), and reports throughput, collections, stalls, RSS and
workload-specific counters.

    python workloads.py list
    python workloads.py run NAME [--threads N] [--duration S | --ops N]
                                 [--scale X] [--acyclic F] [--medium N]
    python workloads.py all [same options]

Runs last --duration seconds (default 30: shorter runs of the steady-state
workloads may never reach a full collection) or, with --ops N, until N steps
are done (the same work, and the same heap growth, for every configuration).

--acyclic F adds acyclic garbage (request-like dicts, lists and tuples, freed
by reference counting) to every step, so that a fraction F of the GC objects a
step leaves behind is acyclic: real programs free most of their garbage by
reference counting. The harness measures a step's cyclic garbage before the
run. --medium N makes every worker keep a small cyclic object graph per step
for N steps (a medium-lived component, like sessions or requests in flight),
so that some objects survive several young collections and die later.

Workloads:

  ext_buffers_malloc, ext_buffers_pymem
                  Extension objects (C type with GC support) that hold large
                  buffers allocated on the side, forming cycles with Python
                  objects. The GC triggers on object counts, so the memory
                  held by garbage is invisible to it; reports the peak of
                  side-buffer bytes. One benchmark takes every buffer from
                  malloc() (memory of a wrapped C library), the other from
                  PyMem_Malloc(); they differ only in the allocator.
                  (ext_buffers is a mix, two malloc() buffers and one
                  PyMem_Malloc() per step.)
  hidden_refs     "Connections" that register a bound method with a C-level
                  holder that has no GC support (like a callback registration
                  or a capsule), which creates a cycle the GC can't see.
                  Most are closed properly; a few are not and leak, so the
                  heap grows over time as in a slow production leak. The
                  leak is fixed per unit of time (1,000 connections a
                  second), or per step with --ops (1 in 100), so the heap
                  grows the same way whatever the throughput.
  c_globals       Records cached by an extension in a C static global list
                  (owned references the GC is never told about), read by the
                  workers while they create ordinary cyclic garbage. Such
                  objects are not reachable from any root the collector knows.
  py_globals      The same records held in a Python module global instead.
                  c_globals means something only next to py_globals: run
                  both.
  orm_session     ORM-style units of work: a session with a weak identity map
                  (WeakValueDictionary), model objects with __slots__,
                  parent/children back-references and a back-reference to the
                  session. Every request loads rows, links them and discards
                  the session: cyclic garbage with weakrefs and callbacks.
  async_server    An asyncio event loop per thread handling batches of
                  requests: coroutines, async generators (some closed early),
                  and caught exceptions kept in a local, which creates
                  exception -> traceback -> frame -> exception cycles.
  event_widgets   A GUI/event-emitter style object tree: widgets with parent
                  and children links, listeners registered as bound methods,
                  lambdas closing over self and functools.partial objects.
  xml_dom         XML processing with the stdlib's xml.dom.minidom: parse a
                  document, query and modify it, serialize it. minidom nodes
                  link to their parent and children, so every discarded
                  document is cyclic garbage (unless unlink() is called,
                  which real code rarely does).
  json_churn      Document processing (json.loads, transform, json.dumps)
                  next to a large long-lived configuration. The garbage is
                  acyclic and freed by reference counting, so allocations and
                  deallocations balance and automatic collections should not
                  trigger at all: a control for GC overhead.
  web_service     A web service with realistic proportions: a large mixed
                  long-lived heap built gradually (configuration, routes,
                  templates, an ORM-style model cache: about 2 million
                  objects at --scale 1), an LRU cache with turnover, sessions
                  that live for a number of requests, and request handling
                  that leaves mostly acyclic garbage, with a few percent of
                  cyclic garbage (exceptions kept in a local, request objects
                  with back-references). Meant to be run for 60 s, and with
                  4 to 8 threads as a thread pool.

event_widgets, orm_session and xml_dom (and async_server) are one family:
a static long-lived heap plus short-lived, almost entirely cyclic garbage.
They differ in object types, not in what they ask of the collector, so treat
them as one piece of evidence, not three.
"""

import argparse
import asyncio
import collections
import functools
import gc
import json
import os
import random
import resource
import statistics
import subprocess
import sys
import sysconfig
import threading
import time
import weakref

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gcstats import GCMonitor, pct, rss_mb   # noqa: E402

FREE_THREADED = bool(sysconfig.get_config_var("Py_GIL_DISABLED"))
perf_ns = time.perf_counter_ns

_ext = None


def ext():
    global _ext
    if _ext is None:
        import buildext
        _ext = buildext.load()
    return _ext


class Workload:
    """Base class: setup() builds shared state, make_step(idx) returns a
    callable doing one unit of work in worker thread `idx`."""
    name = None
    uses_ext = False
    # The results that matter most for this workload, printed first.
    primary = ("ops_per_s", "gc_ms_p99")

    def __init__(self, scale, threads=1, fixed_ops=False):
        self.scale = scale
        self.threads = threads
        self.fixed_ops = fixed_ops

    def n(self, base):
        return max(1, int(base * self.scale))

    def setup(self):
        pass

    def make_step(self, idx):
        raise NotImplementedError

    def release(self, step):
        """Free resources of a step no longer used (the calibration's)."""

    def metrics(self):
        return {}


WORKLOADS = {}


def workload(cls):
    WORKLOADS[cls.name] = cls
    return cls


# ---------------------------------------------------------------------------

class _ExtBuffers(Workload):
    uses_ext = True
    # ops/s depends on the allocator as much as on the collector (read it
    # next to page_faults); what the GC decides is how much side memory
    # garbage holds.
    primary = ("side_peak_mb", "rss_peak_mb")
    # Which allocator the buffers come from: PyMem_Malloc() if true,
    # malloc() if false; one flag for each of the three nodes of a step.
    pymem = (False, False, False)

    def setup(self):
        m = ext()
        p0, p1, _ = self.pymem
        # Long-lived: trees of nodes with small buffers (a model's arrays).
        self.live = []
        for i in range(self.n(60_000)):
            root = m.Node(256, pymem=(p1 if i % 2 == 0 else p0))
            root.a = m.Node(256, pymem=p0)
            root.b = {"name": f"layer{i}", "shape": (16, 16)}
            self.live.append(root)
        m.reset_peak()

    def make_step(self, idx):
        m = ext()
        rng = random.Random(idx)
        Node = m.Node
        p0, p1, p2 = self.pymem

        def step():
            # A "tensor" graph for one request: buffers of 2-16 KiB, linked
            # with Python metadata into a cycle (node -> dict -> node).
            a = Node(rng.randrange(2048, 16384), pymem=p0)
            b = Node(rng.randrange(2048, 16384), pymem=p1)
            c = Node(rng.randrange(2048, 16384), pymem=p2)
            meta = {"inputs": [a, b], "op": "matmul"}
            a.a = b
            b.a = c
            c.a = a
            c.b = meta
        return step

    def metrics(self):
        s = ext().stats()
        return {"side_peak_mb": s["side_peak"] / 2**20,
                "side_now_mb": s["side_bytes"] / 2**20}


@workload
class ExtBuffersMalloc(_ExtBuffers):
    name = "ext_buffers_malloc"


@workload
class ExtBuffersPyMem(_ExtBuffers):
    name = "ext_buffers_pymem"
    pymem = (True, True, True)


@workload
class ExtBuffersMixed(_ExtBuffers):
    # Both allocators: two malloc() buffers and one from PyMem_Malloc()
    # per step. Not in the suite.
    name = "ext_buffers"
    pymem = (False, True, False)


@workload
class HiddenRefs(Workload):
    name = "hidden_refs"
    uses_ext = True
    leak_per_s = 1000       # connections leaked per second (all threads)
    leak_every = 100        # with --ops: 1 in 100 connections leaks
    primary = ("ops_per_s", "leaked_holders", "rss_peak_mb")

    def setup(self):
        self.pool = {"name": "pool", "connections": 0}

    def make_step(self, idx):
        m = ext()
        rng = random.Random(idx)
        pool = self.pool

        class Connection:
            def __init__(self):
                self.pool = pool
                self.buffer = bytearray(256)
                self.headers = {"host": "example.org", "id": rng.random()}
                # The C library keeps a reference to our callback: a cycle
                # self -> handle -> bound method -> self, invisible to the GC.
                self.handle = m.Holder(self.on_readable, 512)
                # And an ordinary cycle the GC can see.
                self.requests = [self]

            def on_readable(self):
                return len(self.buffer)

            def close(self):
                self.handle.close()

        # Leak at a fixed rate, so the heap grows the same way whatever the
        # throughput: per step with --ops, otherwise per unit of time.
        count = [0]
        interval = self.threads / self.leak_per_s
        next_leak = [time.perf_counter() + interval * rng.random()]
        fixed_ops, leak_every = self.fixed_ops, self.leak_every

        def step():
            conn = Connection()
            conn.on_readable()
            count[0] += 1
            if fixed_ops:
                leak = count[0] % leak_every == 0
            else:
                now = time.perf_counter()
                leak = now >= next_leak[0]
                if leak:
                    next_leak[0] += interval
            if not leak:
                conn.close()        # correct use: breaks the hidden cycle
            # else: forgotten, leaks forever
        return step

    def metrics(self):
        s = ext().stats()
        return {"leaked_holders": s["live_holders"],
                "side_now_mb": s["side_bytes"] / 2**20}


class _Globals(Workload):
    uses_ext = True
    where = None

    def setup(self):
        m = ext()
        self.count = self.n(150_000)
        records = []
        for i in range(self.count):
            rec = {"id": i, "tags": [f"t{i % 7}", f"g{i % 13}"],
                   "attrs": {"size": i % 100, "owner": f"user{i % 1000}"}}
            records.append(rec)
        if self.where == "c":
            m.clear_registry()
            for rec in records:
                m.register(rec)
            self.get = m.registry_get
        else:
            _PY_REGISTRY[:] = records
            self.get = _PY_REGISTRY.__getitem__
        del records

    def make_step(self, idx):
        rng = random.Random(idx)
        get = self.get
        count = self.count

        def step():
            # Look up cached records and build a short-lived, cyclic result.
            recs = [get(rng.randrange(count)) for _ in range(8)]
            result = {"records": recs, "total": sum(r["attrs"]["size"] for r in recs)}
            result["self"] = result
        return step

    def metrics(self):
        return {"records": self.count}


_PY_REGISTRY = []


@workload
class CGlobals(_Globals):
    name = "c_globals"
    where = "c"


@workload
class PyGlobals(_Globals):
    name = "py_globals"
    where = "py"


@workload
class OrmSession(Workload):
    name = "orm_session"

    class Model:
        __slots__ = ("id", "parent", "children", "fields", "session",
                     "__weakref__")

        def __init__(self, session, row):
            self.id = row[0]
            self.parent = None
            self.children = []
            self.fields = dict(zip(("name", "price", "qty"), row[2:]))
            self.session = session

    class Session:
        def __init__(self, db):
            self.db = db
            self.identity = weakref.WeakValueDictionary()
            self.new = []

        def get(self, key):
            obj = self.identity.get(key)
            if obj is None:
                obj = OrmSession.Model(self, self.db[key])
                self.identity[key] = obj
                self.new.append(obj)
            return obj

    def setup(self):
        rng = random.Random(0)
        size = self.n(200_000)
        self.db = [(i, rng.randrange(i) if i else None, f"item{i}",
                    rng.random() * 100, rng.randrange(10))
                   for i in range(size)]

    def make_step(self, idx):
        rng = random.Random(idx)
        db = self.db
        Session = self.Session

        def step():
            session = Session(db)
            for _ in range(60):
                obj = session.get(rng.randrange(len(db)))
                parent_id = db[obj.id][1]
                if parent_id is not None:
                    parent = session.get(parent_id)
                    obj.parent = parent
                    parent.children.append(obj)
            total = sum(o.fields["price"] * o.fields["qty"] for o in session.new)
            return total
        return step


@workload
class AsyncServer(Workload):
    name = "async_server"

    def setup(self):
        self.catalog = {i: {"id": i, "name": f"product{i}", "stock": i % 17}
                        for i in range(self.n(50_000))}

    def make_step(self, idx):
        catalog = self.catalog
        size = len(catalog)
        rng = random.Random(idx)
        loop = asyncio.new_event_loop()

        async def lookup(key):
            await asyncio.sleep(0)
            if key % 5 == 0:
                raise KeyError(key)
            return catalog[key % size]

        async def stream(item):
            for part in ("header", "body", "footer"):
                await asyncio.sleep(0)
                yield f"{part}:{item['name']}"

        async def handler(i):
            request = {"id": i, "path": f"/item/{i}", "headers": {"accept": "*/*"}}
            try:
                item = await lookup(i)
            except KeyError as e:
                err = e     # keeps exception -> traceback -> frame -> err
                return {"status": 404, "request": request, "error": repr(err)}
            chunks = []
            async for chunk in stream(item):
                chunks.append(chunk)
                if i % 3 == 0:
                    break   # leaves the async generator to be finalized
            return {"status": 200, "request": request, "body": chunks}

        async def batch():
            base = rng.randrange(1_000_000)
            return await asyncio.gather(*(handler(base + j) for j in range(20)))

        def step():
            loop.run_until_complete(batch())
        step.loop = loop
        return step

    def release(self, step):
        step.loop.close()


@workload
class EventWidgets(Workload):
    name = "event_widgets"

    class Emitter:
        def __init__(self):
            self.listeners = collections.defaultdict(list)

        def on(self, event, callback):
            self.listeners[event].append(callback)

        def emit(self, event, *args):
            for cb in self.listeners[event]:
                cb(*args)

    class Widget:
        def __init__(self, emitter, parent=None):
            self.emitter = emitter
            self.parent = parent
            self.children = []
            self.state = {"clicks": 0, "visible": True}
            if parent is not None:
                parent.children.append(self)
            emitter.on("click", self.on_click)
            emitter.on("render", lambda frame: self.render(frame))
            self.refresh = functools.partial(self.update, "refresh")

        def on_click(self, x, y):
            self.state["clicks"] += 1

        def render(self, frame):
            return (frame, self.state["visible"])

        def update(self, reason):
            self.state["reason"] = reason

    def setup(self):
        # A long-lived main window.
        emitter = self.Emitter()
        root = self.Widget(emitter)
        for _ in range(self.n(40_000)):
            self.Widget(emitter, root)
        self.main_window = root

    def make_step(self, idx):
        Emitter, Widget = self.Emitter, self.Widget

        def step():
            # Open a dialog, interact, close it.
            emitter = Emitter()
            dialog = Widget(emitter)
            for _ in range(5):
                row = Widget(emitter, dialog)
                for _ in range(4):
                    Widget(emitter, row).refresh()
            emitter.emit("click", 10, 20)
            emitter.emit("render", 1)
        return step


@workload
class XmlDom(Workload):
    name = "xml_dom"

    def setup(self):
        rng = random.Random(0)
        docs = []
        for i in range(32):
            rows = "".join(
                f'<row id="{j}" status="{rng.choice(["ok", "late", "void"])}">'
                f"<name>item {rng.randrange(10_000)}</name>"
                f"<amount currency=\"EUR\">{rng.random() * 100:.2f}</amount>"
                f"</row>"
                for j in range(25))
            docs.append(f'<?xml version="1.0"?><report id="{i}">{rows}</report>')
        self.docs = docs
        # Long-lived: a parsed template kept around, as applications do.
        from xml.dom import minidom
        self.templates = [minidom.parseString(docs[i % 8]) for i in range(8 * self.n(40))]

    def make_step(self, idx):
        from xml.dom import minidom
        docs = self.docs
        rng = random.Random(idx)

        def step():
            dom = minidom.parseString(docs[rng.randrange(len(docs))])
            total = 0.0
            for row in dom.getElementsByTagName("row"):
                if row.getAttribute("status") == "void":
                    row.parentNode.removeChild(row)
                    continue
                amount = row.getElementsByTagName("amount")[0]
                total += float(amount.firstChild.data)
            summary = dom.createElement("total")
            summary.appendChild(dom.createTextNode(f"{total:.2f}"))
            dom.documentElement.appendChild(summary)
            return dom.toxml()
        return step


@workload
class JsonChurn(Workload):
    name = "json_churn"

    def setup(self):
        rng = random.Random(0)
        self.config = {
            f"service{i}": {"hosts": [f"10.0.{i % 256}.{j}" for j in range(4)],
                            "limits": {"cpu": i % 8, "mem": i % 64},
                            "tags": (f"t{i % 5}", f"r{i % 3}")}
            for i in range(self.n(60_000))}
        docs = []
        for i in range(64):
            doc = {"order": i, "items": [
                {"sku": f"sku{rng.randrange(10_000)}", "qty": rng.randrange(1, 5),
                 "price": round(rng.random() * 100, 2),
                 "tags": [f"tag{rng.randrange(20)}" for _ in range(3)]}
                for _ in range(20)],
                "customer": {"id": rng.randrange(1_000_000), "country": "NL"}}
            docs.append(json.dumps(doc))
        self.docs = docs

    def make_step(self, idx):
        docs = self.docs
        config = self.config
        rng = random.Random(idx)

        def step():
            doc = json.loads(docs[rng.randrange(len(docs))])
            items = sorted(doc["items"], key=lambda it: it["price"])
            tags = {t for it in items for t in it["tags"]}
            summary = {
                "order": doc["order"],
                "total": sum(it["qty"] * it["price"] for it in items),
                "skus": tuple(it["sku"] for it in items),
                "tags": sorted(tags),
                "limits": config[f"service{doc['order'] % len(config)}"]["limits"],
            }
            return json.dumps(summary)
        return step


@workload
class WebService(Workload):
    name = "web_service"
    primary = ("ops_per_s", "gc_ms_p99", "gc_ms_max", "rss_peak_mb")

    class Model:
        __slots__ = ("id", "name", "price", "category", "related", "attrs")

    class TemplateNode:
        def __init__(self, parent, tag, text=""):
            self.parent = parent
            self.tag = tag
            self.text = text
            self.children = []
            if parent is not None:
                parent.children.append(self)

    class Session:
        def __init__(self, sid, user, lifetime):
            self.id = sid
            self.user = user
            self.remaining = lifetime
            self.data = {"cart": [], "visits": 0}
            self.handlers = [self.on_request]      # session -> bound method -> session

        def on_request(self, path):
            self.data["visits"] += 1
            self.remaining -= 1

    class Request:
        def __init__(self, path, headers):
            self.path = path
            self.headers = headers
            self.context = {"request": self}       # request <-> context

    def setup(self):
        rng = random.Random(0)
        Model, Node = self.Model, self.TemplateNode
        self.config = {}
        self.routes = {}
        self.templates = []
        self.models = []
        self.users = []
        n_models = self.n(300_000)
        # Built gradually and interleaved, with temporary garbage in between,
        # as an application's state is during startup and warm-up.
        for i in range(n_models):
            m = Model()
            m.id = i
            m.name = f"product {i}"
            m.price = round(rng.random() * 100, 2)
            m.category = f"cat{i % 40}"
            m.related = [self.models[rng.randrange(i)]] if i else []
            m.attrs = {"stock": i % 13, "tags": (f"t{i % 7}", f"s{i % 11}")}
            self.models.append(m)
            if i % 15 == 0:
                key = f"service{i}"
                self.config[key] = {"hosts": [f"10.0.{i % 256}.{j}" for j in range(3)],
                                    "limits": {"cpu": i % 8, "mem": i % 64}}
            if i % 60 == 0:
                route = f"/api/v1/items/{i}"
                self.routes[route] = (route, lambda req, i=i: i, {"methods": ["GET"]})
            if i % 150 == 0:
                root = Node(None, "html")
                for b in range(8):
                    block = Node(root, "div", f"block {b}")
                    for c in range(6):
                        Node(block, "span", f"{{{{ item.f{c} }}}}")
                self.templates.append(root)
            if i % 25 == 0:
                self.users.append({"id": i, "name": f"user{i}", "roles": ["reader"],
                                   "prefs": {"lang": "en", "tz": "UTC"}})
            if i % 3 == 0:
                tmp = {"scratch": [i, str(i)], "seq": (i, i + 1)}   # startup garbage
                del tmp
        self.route_keys = list(self.routes)
        self.config_keys = list(self.config)
        self.cache_capacity = self.n(60_000)
        self.lifetime = 50          # requests per session

    def make_step(self, idx):
        rng = random.Random(idx)
        models, users, templates = self.models, self.users, self.templates
        route_keys, routes = self.route_keys, self.routes
        config, config_keys = self.config, self.config_keys
        Session, Request = self.Session, self.Request
        # Per-worker shards (no locking needed): an LRU cache with turnover
        # and the sessions this worker serves.
        capacity = max(1, self.cache_capacity // self.threads)
        cache = collections.OrderedDict()
        sessions = {}
        stats = self._stats = getattr(self, "_stats", {"hits": 0, "misses": 0})
        count = [0]
        lifetime = self.lifetime

        def build_value(key):
            m = models[key % len(models)]
            value = {"id": m.id, "name": m.name, "price": m.price,
                     "related": [r.id for r in m.related], "rendered": f"<li>{m.name}</li>"}
            if key % 3 == 0:
                value["owner"] = value              # some cached values are cyclic
            return value

        def step():
            count[0] += 1
            n = count[0]
            # Parse the request (acyclic garbage).
            route = route_keys[rng.randrange(len(route_keys))]
            headers = {"host": "shop.example", "accept": "text/html",
                       "cookie": f"sid={rng.randrange(1_000_000)}",
                       "x-request-id": f"{idx}-{n}"}
            query = [tuple(kv.split("=")) for kv in ("page=1", f"q=item{n % 97}", "sort=price")]
            req = Request(route, headers) if n % 10 == 0 else None   # cyclic, 1 in 10
            # Session: created, used for `lifetime` requests, then expired.
            sid = rng.randrange(lifetime * 20)
            sess = sessions.get(sid)
            if sess is None:
                sess = sessions[sid] = Session(sid, users[sid % len(users)], lifetime)
            sess.handlers[0](route)
            if sess.remaining <= 0:
                del sessions[sid]                   # medium-lived cyclic garbage
            # Cached lookups with turnover.
            items = []
            for _ in range(4):
                key = rng.randrange(capacity * 4)
                value = cache.get(key)
                if value is None:
                    stats["misses"] += 1
                    value = cache[key] = build_value(key)
                    if len(cache) > capacity:
                        cache.popitem(last=False)   # evicted: old garbage
                else:
                    stats["hits"] += 1
                    cache.move_to_end(key)
                items.append(value)
            # Handle and render (acyclic garbage).
            handler = routes[route][1]
            tpl = templates[n % len(templates)]
            limits = config[config_keys[n % len(config_keys)]]["limits"]
            body = [f"<{node.tag}>{node.text}</{node.tag}>" for node in tpl.children]
            rows = [{"id": v["id"], "name": v["name"], "price": v["price"]} for v in items]
            response = {"status": 200, "route": handler(req), "rows": rows,
                        "limits": dict(limits), "query": dict(query),
                        "body": "".join(body)}
            if n % 20 == 0:
                try:
                    raise ValueError(f"bad input {n}")
                except ValueError as e:
                    err = e         # exception -> traceback -> frame -> err
                    response = {"status": 400, "error": repr(err)}
            return json.dumps(response)
        return step

    def metrics(self):
        st = getattr(self, "_stats", {"hits": 0, "misses": 0})
        total = st["hits"] + st["misses"]
        return {"cache_hit_rate": st["hits"] / total if total else 0.0,
                "live_models": len(self.models)}


# ---------------------------------------------------------------------------

def run_at_depth(depth, fn):
    """Call fn() `depth` Python frames deep, each frame holding a few objects
    in locals (the collector scans every frame of every thread's stack)."""
    if depth <= 0:
        return fn()
    items = [depth, str(depth)]
    table = {"depth": depth, "items": items}
    pair = (items, table)
    return run_at_depth(depth - 1, fn)


class _Medium:
    """A small cyclic object graph kept for a number of steps (--medium)."""
    __slots__ = ("step", "data", "owner", "__weakref__")

    def __init__(self, step):
        self.step = step
        self.data = {"seen": [step], "flags": (step & 1, step & 2)}
        self.owner = self


def _acyclic_unit(i):
    """Request-like acyclic garbage: 6 GC objects, freed by refcounting."""
    record = {"id": i, "tags": ["a", "b"], "items": [(i, i + 1), (i, i + 2)],
              "meta": {"k": i}}
    return len(record)


ACYCLIC_UNIT_OBJECTS = 6


def cyclic_per_step(wl, steps=300):
    """GC objects a step leaves as cyclic garbage (measured with automatic
    collection disabled)."""
    step = wl.make_step(10_000)
    gc.collect()
    gc.disable()
    try:
        before = len(gc.get_objects())
        for _ in range(steps):
            step()
        after = len(gc.get_objects())
    finally:
        gc.enable()
    wl.release(step)
    del step
    gc.collect()
    return max(0.0, (after - before) / steps)


def run(name, threads, duration, scale, gap_threshold_us=200, ticker=True,
        stack_depth=0, ops=None, acyclic=0.0, medium=0):
    use_ticker = ticker
    wl = WORKLOADS[name](scale, threads=threads, fixed_ops=ops is not None)
    t0 = perf_ns()
    wl.setup()
    cyclic = cyclic_per_step(wl)
    gc.collect()
    setup_s = (perf_ns() - t0) / 1e9
    rss_setup = rss_mb()["VmRSS"]
    # Acyclic units per step for a fraction `acyclic` of acyclic GC objects.
    units = (cyclic * acyclic / (1 - acyclic) / ACYCLIC_UNIT_OBJECTS
             if 0 < acyclic < 1 else 0.0)

    stop = threading.Event()
    mon = GCMonitor()
    counts = [0] * threads
    errors = []
    per_worker = (ops + threads - 1) // threads if ops else None

    def worker(idx):
        try:
            step = wl.make_step(idx)
            keep = collections.deque(maxlen=medium) if medium else None
            owed = 0.0
            n = 0
            while not stop.is_set():
                for _ in range(10):
                    step()
                    if units:
                        owed += units
                        while owed >= 1:
                            _acyclic_unit(n)
                            owed -= 1
                    if keep is not None:
                        keep.append(_Medium(n))
                    n += 1
                    if per_worker is not None and n >= per_worker:
                        break
                if per_worker is not None and n >= per_worker:
                    break
            counts[idx] = n
        except BaseException as exc:    # report instead of hanging
            errors.append(repr(exc))
            stop.set()

    gaps = []

    def ticker():
        last = perf_ns()
        threshold = gap_threshold_us * 1000
        while not stop.is_set():
            for _ in range(1000):
                now = perf_ns()
                if now - last > threshold:
                    gaps.append(now - last)
                last = now

    rss_peak = [0.0]
    rss_series = []

    def sampler():
        last = perf_ns()
        while not stop.wait(0.05):
            rss = rss_mb()["VmRSS"]
            rss_peak[0] = max(rss_peak[0], rss)
            if perf_ns() - last >= 1e9:
                rss_series.append(round(rss))
                last = perf_ns()

    mon.start()
    # Marks the measured part for external GC statistics (an audit hook).
    sys.audit("gcbench.measure", "start")
    if stack_depth:
        sys.setrecursionlimit(stack_depth + 1000)
    workers = [threading.Thread(target=run_at_depth,
                                args=(stack_depth, lambda i=i: worker(i)))
               for i in range(threads)]
    helpers = [threading.Thread(target=sampler)]
    if use_ticker:
        helpers.append(threading.Thread(target=ticker))
    faults = resource.getrusage(resource.RUSAGE_SELF).ru_minflt
    start = perf_ns()
    for t in workers + helpers:
        t.start()
    if per_worker is None:
        stop.wait(duration)
        stop.set()
    for t in workers:
        t.join()
    stop.set()
    for t in helpers:
        t.join()
    elapsed = (perf_ns() - start) / 1e9
    faults = resource.getrusage(resource.RUSAGE_SELF).ru_minflt - faults
    sys.audit("gcbench.measure", "stop")
    mon.stop()
    if errors:
        raise RuntimeError(f"{name}: worker failed: {errors[0]}")

    result = {
        "workload": name, "free_threaded": FREE_THREADED,
        "threads": threads, "scale": scale, "duration_s": elapsed,
        "ops": sum(counts), "acyclic": acyclic, "medium": medium,
        "setup_s": setup_s, "ops_per_s": sum(counts) / elapsed,
        **mon.summary(),
        "gc_share": mon.total / elapsed,
        "cyclic_objs_per_step": round(cyclic, 1),
        "acyclic_units_per_step": round(units, 2),
        "rss_setup_mb": rss_setup,
        "rss_peak_mb": max(rss_peak[0], rss_mb()["VmRSS"]),
        "rss_end_mb": rss_mb()["VmRSS"],
        "rss_series_mb": rss_series,
        # Minor page faults in the measured part: memory handed back to the
        # OS and taken again shows up here.
        "page_faults": faults,
        "stack_depth": stack_depth,
        "ticker": use_ticker,
    }
    gc.collect()        # workload counters below count garbage otherwise
    if use_ticker:
        result.update({"stall_ms_p50": pct(gaps, 50) / 1e6,
                       "stall_ms_max": max(gaps) / 1e6 if gaps else 0.0,
                       "stall_ms_total": sum(gaps) / 1e6})
    result.update(wl.metrics())
    result["primary"] = list(WORKLOADS[name].primary)
    return result


def _fmt(k, v):
    return f"{k}={v:.1f}" if isinstance(v, float) else f"{k}={v}"


def show(r):
    primary = "  ".join(_fmt(k, r[k]) for k in r.get("primary", ()) if k in r)
    extra = {k: v for k, v in r.items() if k not in _COMMON and k not in r.get("primary", ())}
    extra_s = "  ".join(_fmt(k, v) for k, v in extra.items())
    line = (f"{r['workload']:14s} {primary}  |  {r['ops_per_s']:10.0f} ops/s  "
            f"gc {r['gc_share'] * 100:4.1f}% in {r['collections']:4d} x "
            f"({r['young_collections']} young, {r['full_collections']} full; "
            f"med {r['gc_ms_median']:6.2f}, p99 {r['gc_ms_p99']:6.2f}, max {r['gc_ms_max']:7.2f} ms)  ")
    if r.get("ticker"):
        line += f"stall max {r['stall_ms_max']:7.1f} ms, total {r['stall_ms_total']:6.0f} ms  "
    line += f"rss {r['rss_setup_mb']:5.0f} -> peak {r['rss_peak_mb']:5.0f} MB  {extra_s}"
    print(line)


_COMMON = {"workload", "free_threaded", "threads", "scale", "duration_s", "ops",
           "acyclic", "medium", "stack_depth", "ticker", "primary",
           "setup_s", "ops_per_s", "gc_mode", "collections", "young_collections",
           "full_collections", "gc_s", "gc_ms_median", "gc_ms_p99", "gc_ms_max",
           "young_ms_median", "young_ms_max", "full_ms_median", "full_ms_max",
           "full_s", "collected", "gc_share", "cyclic_objs_per_step",
           "acyclic_units_per_step", "stall_ms_p50", "stall_ms_max",
           "stall_ms_total", "rss_setup_mb", "rss_peak_mb", "rss_end_mb",
           "rss_series_mb"}


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    for cmd in ("run", "all"):
        s = sub.add_parser(cmd)
        if cmd == "run":
            s.add_argument("name", choices=sorted(WORKLOADS))
        s.add_argument("--threads", type=int, default=4)
        s.add_argument("--duration", type=float, default=30.0)
        s.add_argument("--ops", type=int, default=None,
                       help="run this many steps (in total) instead of --duration")
        s.add_argument("--scale", type=float, default=1.0,
                       help="multiply the size of the long-lived data")
        s.add_argument("--acyclic", type=float, default=0.0,
                       help="fraction of a step's garbage objects that is acyclic")
        s.add_argument("--medium", type=int, default=0,
                       help="keep a small cyclic object per step for this many steps")
        s.add_argument("--json", action="store_true")
        s.add_argument("--stack-depth", type=int, default=0,
                       help="run each worker this many Python frames deep")
        s.add_argument("--no-ticker", dest="ticker", action="store_false",
                       help="don't run the stall-measuring thread (it "
                            "competes for the GIL in the default build)")
    args = p.parse_args()

    if args.cmd == "list":
        for name, cls in WORKLOADS.items():
            print(name)
        return
    opts = dict(ticker=args.ticker, stack_depth=args.stack_depth, ops=args.ops,
                acyclic=args.acyclic, medium=args.medium)
    if args.cmd == "run":
        r = run(args.name, args.threads, args.duration, args.scale, **opts)
        show(r)
        if args.json:
            print(json.dumps(r))
        return
    # all: each workload in a fresh process
    for name in WORKLOADS:
        cmd = [sys.executable, __file__, "run", name, "--json",
               "--threads", str(args.threads), "--duration", str(args.duration),
               "--scale", str(args.scale), "--acyclic", str(args.acyclic),
               "--medium", str(args.medium)]
        if args.ops:
            cmd += ["--ops", str(args.ops)]
        if not args.ticker:
            cmd.append("--no-ticker")
        if args.stack_depth:
            cmd += ["--stack-depth", str(args.stack_depth)]
        out = subprocess.run(cmd, capture_output=True, text=True)
        if out.returncode != 0:
            print(f"{name}: FAILED\n{out.stderr[-2000:]}")
            continue
        r = json.loads(out.stdout.strip().splitlines()[-1])
        show(r)
        if args.json:
            print(json.dumps(r))


if __name__ == "__main__":
    main()
