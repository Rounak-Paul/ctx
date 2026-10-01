#!/usr/bin/env python3
"""Python resolution smoke test: module-qualified names, import aliases,
from-imports, attribute types from __init__, annotations and return types,
super() and inherited methods through module-qualified bases, and builtins
that must not bind to same-named project functions."""
import sys

from resolution_harness import Project

FILES = {
    "pkg/__init__.py": "",
    "pkg/models.py": """\
class Engine:
    def flush(self):
        return 1


class Cache(Engine):
    def warm(self):
        return 2


def make_engine() -> Engine:
    return Engine()
""",
    "pkg/other.py": """\
class Engine:
    def flush(self):
        return 3


def len(x):
    return 0


def build():
    return Engine()
""",
    "app.py": """\
import pkg.models as m
from pkg.other import Engine as OtherEngine
from pkg import models
from pkg.other import build


class Service:
    def __init__(self):
        self.engine = m.Engine()
        self.old = OtherEngine()

    def run_field(self):
        return self.engine.flush()

    def run_other(self):
        return self.old.flush()

    def run_local(self):
        e = models.make_engine()
        return e.flush()


class FastCache(m.Cache):
    def warm(self):
        return super().warm()

    def go(self):
        return self.flush()


def run_annot(x: OtherEngine):
    return x.flush()


def builtin_call():
    return len([])


def run_module_fn():
    return m.make_engine()


def run_inferred():
    return build().flush()
""",
}


def main():
    if len(sys.argv) != 2:
        print("usage: python_resolution_smoke.py /path/to/ctx", file=sys.stderr)
        return 2
    with Project(sys.argv[1], FILES, "python-resolution") as p:
        p.callees("Service.run_field", expect=("pkg/models.py:2",), reject=("pkg/other.py:2",))
        p.callees("Service.run_other", expect=("pkg/other.py:2",), reject=("pkg/models.py:2",))
        p.callees("Service.run_local", expect=("pkg/models.py:11", "pkg/models.py:2"))
        p.callees("FastCache.warm", expect=("pkg/models.py:7",))
        p.callees("FastCache.go", expect=("pkg/models.py:2",))
        p.callees("run_annot", expect=("pkg/other.py:2",), reject=("pkg/models.py:2",))
        p.callees("builtin_call", reject=("pkg/other.py:6",))
        p.callees("run_module_fn", expect=("pkg/models.py:11",))
        p.callees("run_inferred", expect=("pkg/other.py:10", "pkg/other.py:2"))
        p.callees("app.Service.run_field", expect=("pkg/models.py:2",))
    print("python_resolution_smoke: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
