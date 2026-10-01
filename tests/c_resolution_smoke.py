#!/usr/bin/env python3
"""C resolution smoke test: same-named functions across translation units
prefer the one whose header is included, static helpers stay in their file,
calls through struct function pointers never bind to unrelated functions
and anonymous typedef'd structs are named by their typedef."""
import sys

from resolution_harness import Project

FILES = {
    "lib_a.h": """\
int init(void);
""",
    "lib_a.c": """\
#include "lib_a.h"
int init(void) { return 1; }
""",
    "lib_b.c": """\
int init(void) { return 2; }
int read(int fd) { return fd; }
""",
    "main.c": """\
#include "lib_a.h"
struct ops { int (*read)(int); };
typedef struct { int (*run)(void); } Runner;
static int helper(void) { return 0; }
int start(struct ops *o) {
    helper();
    o->read(3);
    return init();
}
int run_runner(Runner *r) { return r->run(); }
""",
    "other.c": """\
static int helper(void) { return 5; }
int other_entry(void) { return helper(); }
""",
}


def main():
    if len(sys.argv) != 2:
        print("usage: c_resolution_smoke.py /path/to/ctx", file=sys.stderr)
        return 2
    with Project(sys.argv[1], FILES, "c-resolution") as p:
        p.callees("start", expect=("lib_a.c:2", "main.c:4"), reject=("lib_b.c:1", "lib_b.c:2", "other.c:1"))
        p.callees("other_entry", expect=("other.c:1",), reject=("main.c:4",))
        outline = p.tool("outline", {"path": "main.c"})
        if "Runner" not in outline:
            raise AssertionError(f"anonymous typedef struct not named:\n{outline}")
    print("c_resolution_smoke: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
