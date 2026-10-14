"""Benchmarks distilled from the workloads behind the incremental GC reverts.

The incremental cycle collector was reverted twice for the default build:

* 3.13 (gh-124567): Sphinx became 38-48% slower building the CPython
  docs. The collector formed the transitive closure of the young
  generation before collecting it, and docutils trees link every node to
  its parent and to the document, whose settings link to the build
  environment: the closure of any new node was most of the heap, and
  scanning it on every young collection made the work grow quadratically
  with the heap (gh-124717).

* 3.14.5/3.15 (gh-148726): Django's `migrate` ran out of memory. Each
  migration step renders all models of the project state as new classes
  (`ModelState.render()` calls `type()`), whose fields, options and
  registry refer back to them. These classes live for roughly one step,
  long enough to survive several young collections, then become cyclic
  garbage that the incremental collector reclaimed far too slowly (Tim
  Peters' deque test on discuss.python.org shows the same effect in its
  purest form).

This module contains no Django or Sphinx code; each benchmark reproduces
the object graph and lifetimes of its workload:

  migrations  Load a migration history of --history migrations (long-
              lived; real projects have hundreds to a few thousand
              migrations, so the default 60,000 stands for the rest of a
              large application's long-lived heap, about 1.8 million
              objects, rather than for migrations), then for each of
              --steps migrations clone the project
              state (--models models), apply an operation, render every
              model as a new class with an Options object, fields, a
              manager and a registry (class <-> _meta <-> fields <->
              registry cycles; foreign keys link classes), and run a
              schema editor and data migration that fill caches on the
              previous and new rendered classes with new objects (cycles
              mixing older classes and young objects). Primary results:
              peak RSS and the longest pause. Its wall time is GC-bound
              and not representative of a real migrate.
  doctree     Parse --docs documents (default 4800) into docutils-like node
              trees (parent and document back-links; the document links to
              an environment holding every document's metadata), run
              transforms with temporary visitors, and keep the trees. Each
              document also leaves cyclic garbage, as docutils does: the
              parser's state machine (states linked to the machine and the
              document) and subtrees removed by transforms (system messages
              with children). Primary results: GC time per document as
              the heap grows (gc_ms_per_doc against live_nodes, per
              quarter of the run) and the longest pause.
  deque       Tim Peters' test: --iterations self-referential objects with
              a --payload byte payload pushed through deque(maxlen=1000).
              Primary results: the garbage backlog and peak RSS; the run
              is short and its timings say little.

Every benchmark prints one JSON line: the primary results first, then wall
time, peak RSS, collections (young and full, see gcstats.py), total and
maximum collection time (gc.callbacks), and a garbage backlog measure. --threads N runs N independent copies at once. Works on any
CPython 3.12+, default or free-threaded build.
"""

import argparse
import gc
import json
import random
import sys
import sysconfig
import threading
import time
import weakref
from collections import deque

from gcstats import GCMonitor

perf = time.perf_counter
_MONITOR = None


def rss_mb():
    status = {}
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith(("VmRSS", "VmHWM")):
                k, v = line.split(":")
                status[k] = int(v.split()[0]) / 1024
    return status


# --------------------------------------------------------------- migrations

class Field:
    def __init__(self, name, kind, to=None, **options):
        self.name = name
        self.kind = kind
        self.to = to
        self.options = options
        self.model = None
        self.remote_model = None
        self.validators = [self.validate]
        self.choices = options.get("choices")

    def validate(self, value):
        return value

    def clone(self):
        return Field(self.name, self.kind, self.to, **self.options)

    def contribute_to_class(self, cls, name):
        self.model = cls
        cls._meta.fields.append(self)
        cls._meta.fields_map[name] = self


class Options:
    def __init__(self, model, app_label, apps):
        self.model = model
        self.app_label = app_label
        self.apps = apps
        self.fields = []
        self.fields_map = {}
        self.related_objects = []
        self._cache = {}
        self.verbose_name = model.__name__.lower()

    def get_field(self, name):
        return self.fields_map[name]


class Manager:
    """Model.objects: refers to its model; querysets cache instances."""

    def __init__(self, model):
        self.model = model
        self._result_cache = None

    def all(self, n, rng):
        # A queryset evaluation: instances that refer to their class, cached
        # on the manager, so class -> manager -> instances -> class.
        self._result_cache = [self.model(id=rng.randrange(10**6), f1="x" * 10)
                              for _ in range(n)]
        return self._result_cache


class ModelBase:
    def __init__(self, **kwargs):
        for k, v in kwargs.items():
            setattr(self, k, v)
        self._state = {"db": "default", "adding": True, "model": type(self)}

    def save(self):
        return [f.validate(getattr(self, f.name, None)) for f in self._meta.fields]


class StateApps:
    """The registry Django builds for a rendered project state."""

    def __init__(self):
        self.all_models = {}
        self.ready = False
        self._pending_lookups = []

    def register_model(self, name, model):
        self.all_models[name] = model

    def get_model(self, name):
        return self.all_models[name]


class ModelState:
    def __init__(self, name, fields, options):
        self.name = name
        self.fields = fields
        self.options = options

    def clone(self):
        return ModelState(self.name, {k: f.clone() for k, f in self.fields.items()},
                          dict(self.options))

    def render(self, apps):
        fields = {k: f.clone() for k, f in self.fields.items()}
        body = {
            "__module__": "__fake__",
            "__qualname__": self.name,
            "__doc__": f"Model {self.name}.  " * 20,
            "__str__": lambda self: f"{type(self).__name__} object",
        }
        cls = type(self.name, (ModelBase,), body)
        cls._meta = Options(cls, "app", apps)
        for name, f in fields.items():
            f.contribute_to_class(cls, name)
            # Field descriptors on the class, as Django installs them.
            setattr(cls, name + "_descriptor", property(lambda obj, n=name: obj.__dict__.get(n)))
        cls.objects = Manager(cls)
        apps.register_model(self.name, cls)
        return cls


class Migration:
    """A loaded migration file: its dependencies and operations."""

    def __init__(self, app_label, name, dependencies, operations):
        self.app_label = app_label
        self.name = name
        self.dependencies = dependencies
        self.operations = operations
        self.replaces = []


class AddField:
    def __init__(self, model_name, name, field):
        self.model_name = model_name
        self.name = name
        self.field = field
        self.preserve_default = True


class MigrationLoader:
    """The migration graph Django loads before migrating: every migration of
    every app stays alive for the whole command."""

    def __init__(self, names, count, rng):
        self.disk_migrations = {}
        self.graph = {}
        prev = None
        for i in range(count):
            model = names[rng.randrange(len(names))]
            ops = [AddField(model, f"hist{i}_{j}", Field(f"hist{i}_{j}", "char", max_length=50))
                   for j in range(3)]
            m = Migration("app", f"{i:04d}_auto", [prev] if prev else [], ops)
            self.disk_migrations[("app", m.name)] = m
            self.graph[m.name] = {"node": m, "parents": set(m.dependencies),
                                  "children": set()}
            if prev:
                self.graph[prev]["children"].add(m.name)
            prev = m.name


class ProjectState:
    def __init__(self, models):
        self.models = models

    def clone(self):
        return ProjectState({k: m.clone() for k, m in self.models.items()})

    def render(self):
        apps = StateApps()
        for m in self.models.values():
            m.render(apps)
        for cls in apps.all_models.values():     # resolve relations
            for f in cls._meta.fields:
                if f.to is not None:
                    f.remote_model = apps.get_model(f.to)
                    f.remote_model._meta.related_objects.append(f)
        apps.ready = True
        return apps


def bench_migrations(args, rng):
    names = [f"Model{i}" for i in range(args.models)]
    models = {}
    for i, name in enumerate(names):
        fields = {"id": Field("id", "auto", primary_key=True)}
        for j in range(args.fields):
            if j % 4 == 0 and i > 0:
                fields[f"rel{j}"] = Field(f"rel{j}", "fk", to=names[rng.randrange(i)],
                                          on_delete="CASCADE")
            else:
                fields[f"f{j}"] = Field(f"f{j}", "char", max_length=100,
                                        choices=[(k, str(k)) for k in range(5)])
        models[name] = ModelState(name, fields, {"db_table": name.lower()})
    loader = MigrationLoader(names, args.history, rng)
    state = ProjectState(models)
    live_apps = weakref.WeakSet()
    backlog_max = 0
    from_apps = state.render()
    for step in range(args.steps):
        # Apply an operation (AddField) to a clone of the state and render
        # the new state; the previous rendering is the "from" state.
        to_state = state.clone()
        target = to_state.models[names[rng.randrange(len(names))]]
        fname = f"added{step}"
        target.fields[fname] = Field(fname, "int", default=0)
        to_apps = to_state.render()
        live_apps.add(to_apps)
        # The schema editor and the data migration run for a while and
        # fill caches on the (by now older) rendered classes with new
        # objects: relation trees, field caches, cached querysets.
        for rendered in (from_apps, to_apps):
            for cls in rendered.all_models.values():
                meta = cls._meta
                meta._cache["fields"] = tuple(meta.fields)
                meta._cache["relation_tree"] = [f for f in meta.related_objects]
                meta._cache["sql"] = [f"ALTER TABLE {meta.verbose_name} ADD {f.name}"
                                      for f in meta.fields]
        for _ in range(args.instances):
            cls = to_apps.get_model(names[rng.randrange(len(names))])
            for obj in cls.objects.all(5, rng):
                obj.save()
        state = to_state
        from_apps = to_apps
        backlog_max = max(backlog_max, len(live_apps))
    del loader
    return {"work": args.steps, "backlog_max": backlog_max,
            "backlog_what": "rendered registries alive"}


# ------------------------------------------------------------------ doctree

class Environment:
    """Sphinx's BuildEnvironment: metadata about every document."""

    def __init__(self):
        self.all_docs = {}
        self.titles = {}
        self.toc = {}
        self.domaindata = {"py": {"objects": {}}, "std": {"labels": {}}}
        self.doctrees = {}


class Settings:
    def __init__(self, env):
        self.env = env
        self.language_code = "en"
        self.tab_width = 8


class DocNode:
    tagname = "node"

    def __init__(self, rawsource="", *children, **attributes):
        self.rawsource = rawsource
        self.parent = None
        self.document = None
        self.children = []
        self.attributes = {"ids": [], "classes": [], "names": [],
                           "dupnames": [], "backrefs": []}
        self.attributes.update(attributes)
        for c in children:
            self.append(c)

    def append(self, child):
        child.parent = self
        child.document = self.document
        self.children.append(child)

    def traverse(self, cls=None):
        result = [self] if cls is None or isinstance(self, cls) else []
        for c in self.children:
            result.extend(c.traverse(cls))
        return result


class Text(str):
    tagname = "#text"

    def __new__(cls, data, rawsource=""):
        return str.__new__(cls, data)

    def traverse(self, cls=None):
        return [self] if cls is None or isinstance(self, cls) else []


class document(DocNode):
    tagname = "document"

    def __init__(self, settings, docname):
        super().__init__()
        self.document = self
        self.settings = settings
        self.docname = docname
        self.ids = {}
        self.nameids = {}
        self.transformer = None


class section(DocNode): tagname = "section"
class title(DocNode): tagname = "title"
class paragraph(DocNode): tagname = "paragraph"
class reference(DocNode): tagname = "reference"
class literal(DocNode): tagname = "literal"
class pending(DocNode): tagname = "pending"


class system_message(DocNode): tagname = "system_message"


class State:
    """A docutils parser state: linked to its state machine, which links to
    every state and to the document being parsed."""

    def __init__(self, machine, name):
        self.state_machine = machine
        self.name = name
        self.transitions = {"blank": self.blank, "text": self.text}
        self.nested_sm_kwargs = {"state_classes": None, "initial_state": name}

    def blank(self, match, context):
        return context, self.name

    def text(self, match, context):
        return context + [match], self.name


class StateMachine:
    def __init__(self, document, names):
        self.document = document
        self.states = {n: State(self, n) for n in names}
        self.input_lines = []
        self.memo = {"document": document, "reporter": self, "section_level": 0}


class Visitor:
    def __init__(self, document):
        self.document = document
        self.found = []

    def visit(self, node):
        if isinstance(node, reference):
            self.found.append((node, node.attributes.get("refuri")))


def parse(docname, words, rng, settings):
    doc = document(settings, docname)
    # The parser's state machine is discarded after parsing: cyclic garbage.
    machine = StateMachine(doc, ("Body", "Text", "Line", "Section", "Inliner"))
    for s in range(6):
        sec = section()
        doc.append(sec)
        sec.attributes["ids"].append(f"{docname}-s{s}")
        doc.ids[f"{docname}-s{s}"] = sec
        t = title()
        sec.append(t)
        t.children.append(Text(" ".join(rng.sample(words, 4))))
        for p in range(8):
            para = paragraph()
            sec.append(para)
            for k in range(6):
                if k % 3 == 1:
                    ref = reference(refuri=f"doc{rng.randrange(1000)}.html")
                    para.append(ref)
                    ref.children.append(Text(rng.choice(words)))
                elif k % 3 == 2:
                    lit = literal()
                    para.append(lit)
                    lit.children.append(Text(rng.choice(words)))
                else:
                    para.children.append(Text(" ".join(rng.sample(words, 8))))
            # A pending node resolved by a transform, then dropped.
            pend = pending(details={"kind": "ref"})
            para.append(pend)
            machine.input_lines.append(para)
        # A system message (with children) that a transform removes: a
        # subtree whose nodes link to each other and to their parent.
        msg = system_message(level=1, type="INFO")
        sec.append(msg)
        for k in range(3):
            p2 = paragraph()
            msg.append(p2)
            p2.children.append(Text(" ".join(rng.sample(words, 6))))
    return doc


def transform(doc):
    visitor = Visitor(doc)
    for node in doc.traverse(DocNode):
        visitor.visit(node)
        node.children = [c for c in node.children
                         if not isinstance(c, (pending, system_message))]
    return visitor.found


NODES_PER_DOC = 6 * (2 + 8 * 5) + 1     # sections, titles, paragraphs, refs, literals


def bench_doctree(args, rng):
    words = [f"word{i}" for i in range(2000)]
    env = Environment()
    settings = Settings(env)
    t0 = perf()
    checkpoints = []
    marks = {args.docs * q // 4 for q in range(1, 5)}
    for i in range(1, args.docs + 1):
        docname = f"doc{i}"
        doc = parse(docname, words, rng, settings)
        refs = transform(doc)
        env.all_docs[docname] = time.time()
        env.titles[docname] = doc.children[0].children[0]
        env.toc[docname] = [s.attributes["ids"][0] for s in doc.children]
        for node, uri in refs:
            env.domaindata["std"]["labels"].setdefault(uri, []).append(docname)
        env.doctrees[docname] = doc   # kept for the write phase
        if i in marks:
            checkpoints.append((i, perf() - t0, _MONITOR.total))
    # Per quarter of the run: the live tree nodes at its end, and the time
    # and GC time per document in it. GC time per document grows with the
    # live heap if every collection scans it.
    quarters = []
    last_n, last_t, last_gc = 0, 0.0, 0.0
    for n, t, g in checkpoints:
        docs = max(n - last_n, 1)
        quarters.append({"live_nodes": n * NODES_PER_DOC,
                         "ms_per_doc": round((t - last_t) / docs * 1e3, 3),
                         "gc_ms_per_doc": round((g - last_gc) / docs * 1e3, 3)})
        last_n, last_t, last_gc = n, t, g
    return {"work": args.docs, "quarters": quarters}


# -------------------------------------------------------------------- deque

class SelfRef:
    def __init__(self, payload):
        self.payload = bytes(payload)
        self.me = self


_deque_progress = {}


def bench_deque(args, rng):
    # Tim Peters' test as in nascheme/cyclotron (queue_cycles.py): each
    # object is garbage 1000 iterations after it was created.
    me = threading.get_ident()
    d = deque(maxlen=1000)
    max_trash = 0
    for c in range(1, args.iterations + 1):
        d.append(SelfRef(args.payload))
        if c % 500 == 0:
            _deque_progress[me] = (c, len(d))
            created = sum(x[0] for x in _deque_progress.values())
            live = sum(x[1] for x in _deque_progress.values())
            max_trash = max(max_trash, created - live - _MONITOR.collected)
    return {"work": args.iterations, "backlog_max": max_trash,
            "backlog_what": "uncollected garbage objects"}


BENCHES = {"migrations": bench_migrations, "doctree": bench_doctree,
           "deque": bench_deque}
PRIMARY = {"migrations": ("peak_rss_mb", "gc_max_ms"),
           "doctree": ("quarters", "gc_max_ms"),
           "deque": ("backlog_max", "peak_rss_mb")}


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("bench", choices=sorted(BENCHES))
    p.add_argument("--threads", type=int, default=1)
    p.add_argument("--models", type=int, default=200)
    p.add_argument("--fields", type=int, default=16)
    p.add_argument("--steps", type=int, default=300)
    p.add_argument("--instances", type=int, default=40)
    p.add_argument("--history", type=int, default=60000,
                   help="migrations loaded (long-lived objects)")
    p.add_argument("--docs", type=int, default=4800)
    p.add_argument("--iterations", type=int, default=1500000)
    p.add_argument("--payload", type=int, default=10000)
    args = p.parse_args()

    global _MONITOR
    mon = _MONITOR = GCMonitor().start()
    # Marks the measured part for external GC statistics (an audit hook).
    sys.audit("gcbench.measure", "start")
    peak = [0.0]
    stop = threading.Event()

    def sampler():
        while not stop.wait(0.02):
            peak[0] = max(peak[0], rss_mb()["VmRSS"])

    results = [None] * args.threads
    errors = []

    def run(i):
        try:
            results[i] = BENCHES[args.bench](args, random.Random(i))
        except BaseException as e:
            errors.append(repr(e))

    s = threading.Thread(target=sampler)
    s.start()
    t0 = perf()
    ts = [threading.Thread(target=run, args=(i,)) for i in range(args.threads)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    wall = perf() - t0
    stop.set()
    s.join()
    sys.audit("gcbench.measure", "stop")
    mon.stop()
    if errors:
        sys.exit(errors[0])
    r = dict(results[0])
    if args.threads > 1:
        for k in ("backlog_max",):
            if k in r:
                r[k] = max(x[k] for x in results)
    status = rss_mb()
    s = mon.summary()
    common = {
        "wall_s": round(wall, 3),
        "peak_rss_mb": round(max(peak[0], status["VmHWM"]), 1),
        "gc_max_ms": round(s["gc_ms_max"], 2),
        "gc_s": round(s["gc_s"], 3),
        **{k: v for k, v in s.items() if k not in ("gc_ms_max", "gc_s")},
    }
    # The primary results first.
    primary = PRIMARY[args.bench]
    out = {"bench": args.bench, "threads": args.threads,
           **{k: (r | common)[k] for k in primary}}
    out.update({"python": sys.version.split()[0],
                "free_threaded": bool(sysconfig.get_config_var("Py_GIL_DISABLED"))})
    out.update(common)
    out.update(r)
    print(json.dumps(out))


if __name__ == "__main__":
    main()
