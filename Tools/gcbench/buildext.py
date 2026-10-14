"""Build and import the _gcbench_ext extension for the running interpreter.

Compiles _gcbench_ext.c with the interpreter's compiler and flags (works for
uninstalled build trees, which have no setuptools) into build/<key>/, where
<key> identifies the interpreter, and rebuilds when the source is newer.

    python buildext.py      # build (or check) for this interpreter
"""

import hashlib
import importlib.util
import os
import shlex
import subprocess
import sys
import sysconfig

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "_gcbench_ext.c")


def _output_path():
    key = hashlib.sha1(os.path.realpath(sys.executable).encode()).hexdigest()[:10]
    suffix = sysconfig.get_config_var("EXT_SUFFIX")
    return os.path.join(HERE, "build", key, "_gcbench_ext" + suffix)


def _include_dirs():
    if sysconfig.is_python_build():
        srcdir = sysconfig.get_config_var("srcdir")
        builddir = os.path.dirname(os.path.realpath(sys.executable))
        return [os.path.join(srcdir, "Include"), builddir]
    return [sysconfig.get_paths()["include"]]


def build(force=False):
    out = _output_path()
    if (not force and os.path.exists(out)
            and os.path.getmtime(out) >= os.path.getmtime(SOURCE)):
        return out
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cc = shlex.split(sysconfig.get_config_var("CC") or "cc")
    cflags = shlex.split(sysconfig.get_config_var("CFLAGS") or "")
    ccshared = shlex.split(sysconfig.get_config_var("CCSHARED") or "-fPIC")
    cmd = [*cc, *cflags, *ccshared, "-O2", "-shared",
           *(f"-I{d}" for d in _include_dirs()), SOURCE, "-o", out]
    subprocess.run(cmd, check=True)
    return out


def load():
    """Build if needed and import _gcbench_ext."""
    if "_gcbench_ext" in sys.modules:
        return sys.modules["_gcbench_ext"]
    path = build()
    spec = importlib.util.spec_from_file_location("_gcbench_ext", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["_gcbench_ext"] = mod
    spec.loader.exec_module(mod)
    return mod


if __name__ == "__main__":
    print(build(force="--force" in sys.argv))
