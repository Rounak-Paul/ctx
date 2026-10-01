#!/usr/bin/env python3
"""C++ resolution smoke test: namespaces (nested, anonymous), same-named
functions in different scopes, qualified/member calls, qualified bases,
aliases, typedefs, using-directives through transitive includes, ADL,
receiver typing (locals, fields, smart pointers, auto, call chains and
return types) and incremental re-resolution after a header edit."""
import sys

from resolution_harness import Project

FILES = {
    "geo.cpp": """\
namespace geo {
struct Shape { int sides = 0; };
namespace detail {
int clamp(int v) { return v < 0 ? 0 : v; }
}
class Circle : public Shape {
public:
    int area() const { return detail::clamp(radius); }
    int radius = 0;
};
int measure() { return detail::clamp(3); }
}
namespace {
int helper() { return 1; }
}
int use_helper_geo() { return helper(); }
""",
    "net.cpp": """\
namespace net::detail {
int clamp(int v) { return v > 100 ? 100 : v; }
}
namespace net {
int send_packet() { return detail::clamp(5); }
int route_packet() { return geo::detail::clamp(1); }
int flush_packet() { return std::max(1, 2); }
}
namespace {
int helper() { return 2; }
}
int use_helper_net() { return helper(); }
""",
    "ui.cpp": """\
namespace gfx { int draw() { return 9; } }
namespace ui {
struct Shape { int draw(); };
int Shape::draw() { return 0; }
struct Widget : public geo::Shape {};
int paint_shape(Shape &s) { return s.draw(); }
}
""",
    "lib/store.hpp": """\
namespace storage::v2 {
struct Engine {
    int flush_all();
    int compact_now();
};
struct Cache : Engine {
    int warm_up();
};
int open_db();
int checksum(Engine &e);
}
namespace sv = storage::v2;
using namespace storage::v2;
""",
    "lib/store.cpp": """\
#include "store.hpp"
namespace storage::v2 {
int Engine::flush_all() { return 1; }
int Engine::compact_now() { return 2; }
int Cache::warm_up() { return flush_all(); }
int open_db() { return 3; }
int checksum(Engine &e) { return 4; }
}
""",
    "other.cpp": """\
namespace legacy {
struct Engine { int flush_all(); };
int Engine::flush_all() { return 0; }
int open_db() { return 9; }
int checksum(Engine &e) { return 8; }
}
""",
    "app.cpp": """\
#include "lib/store.hpp"
#include <memory>
namespace app {
using EnginePtr = storage::v2::Engine;
typedef storage::v2::Cache CacheT;
struct Service {
    storage::v2::Engine engine_;
    std::unique_ptr<legacy::Engine> old_;
    int run_alias() { return sv::open_db(); }
    int run_directive() { return open_db(); }
    int run_field() { return engine_.flush_all(); }
    int run_smart() { return old_->flush_all(); }
};
int run_local() {
    EnginePtr e;
    return e.compact_now();
}
int run_typedef_qualifier() { return CacheT::warm_up(); }
int run_base_via_derived(CacheT &c) { return c.flush_all(); }
int run_auto() {
    auto c = std::make_unique<legacy::Engine>();
    return c->flush_all();
}
int run_adl() {
    storage::v2::Engine e;
    return checksum(e);
}
int run_using_decl() {
    using legacy::open_db;
    return open_db();
}
}
""",
}

CHAIN_FILES = {
    "deep/inner.hpp": """\
namespace deep { int ping(); }
using namespace deep;
""",
    "deep/outer.hpp": """\
#include "inner.hpp"
""",
    "deep/inner.cpp": """\
#include "inner.hpp"
namespace deep { int ping() { return 1; } }
""",
    "chain.cpp": """\
#include "deep/outer.hpp"
namespace other { int ping() { return 2; } }
namespace shop {
struct Cache { int warm(); };
struct Engine {
    Cache cache_;
    Cache &cache() { return cache_; }
};
struct Store {
    Engine engine_;
    Engine &engine() { return engine_; }
};
Engine make_engine();
int Cache::warm() { return 3; }
int via_fields(Store &s) { return s.engine_.cache_.warm(); }
int via_methods(Store &s) { return s.engine().cache().warm(); }
int via_auto() {
    auto e = make_engine();
    return e.cache().warm();
}
}
int transitive_using() { return ping(); }
""",
}


def main():
    if len(sys.argv) != 2:
        print("usage: cpp_scope_smoke.py /path/to/ctx", file=sys.stderr)
        return 2
    ctx_bin = sys.argv[1]

    with Project(ctx_bin, FILES, "cpp-scope") as p:
        p.callers("geo::detail::clamp", expect=("area", "measure", "route_packet"), reject=("send_packet",))
        p.callers("net::detail::clamp", expect=("send_packet",), reject=("route_packet", "measure"))
        flush = p.tool("callees", {"symbol": "flush_packet"})
        if "std::max" not in flush or "  L" in flush:
            raise AssertionError(f"std::max should stay unresolved:\n{flush}")
        p.callees("use_helper_geo", expect=("geo.cpp:14",), reject=("net.cpp:10",))
        p.callees("use_helper_net", expect=("net.cpp:10",), reject=("geo.cpp:14",))
        p.callees("paint_shape", expect=("ui.cpp:4",), reject=("ui.cpp:1",))
        area = p.tool("source", {"symbol": "geo::Circle::area"})
        if "detail::clamp(radius)" not in area:
            raise AssertionError(f"qualified method lookup failed:\n{area}")
        impact = p.tool("impact", {"symbol": "geo::Shape"})
        if "Circle" not in impact or "Widget" not in impact:
            raise AssertionError(f"qualified bases not linked to geo::Shape:\n{impact}")
        if "Widget" in p.tool("impact", {"symbol": "ui::Shape"}):
            raise AssertionError("geo::Shape base bound to ui::Shape")

        p.callees("app::Service::run_alias", expect=("lib/store.cpp:6",))
        p.callees("app::Service::run_directive", expect=("lib/store.cpp:6",))
        p.callees("app::Service::run_field", expect=("lib/store.cpp:3",))
        p.callees("app::Service::run_smart", expect=("other.cpp:3",))
        p.callees("app::run_local", expect=("lib/store.cpp:4",))
        p.callees("app::run_typedef_qualifier", expect=("lib/store.cpp:5",))
        p.callees("app::run_base_via_derived", expect=("lib/store.cpp:3",))
        p.callees("app::run_auto", expect=("other.cpp:3",))
        p.callees("app::run_adl", expect=("lib/store.cpp:7",))
        p.callees("app::run_using_decl", expect=("other.cpp:4",))
        p.callees("storage::v2::Cache::warm_up", expect=("lib/store.cpp:3",))

        p.write("lib/store.hpp", FILES["lib/store.hpp"].replace("namespace sv = storage::v2;",
                                                                 "namespace sv = legacy;"))
        p.wait_for_callee("app::Service::run_alias", "other.cpp:4")

    with Project(ctx_bin, CHAIN_FILES, "cpp-chain") as p:
        p.callees("shop::via_fields", expect=("chain.cpp:14",))
        p.callees("shop::via_methods", expect=("chain.cpp:14", "chain.cpp:7", "chain.cpp:11"))
        p.callees("shop::via_auto", expect=("chain.cpp:14", "chain.cpp:7"))
        p.callees("transitive_using", expect=("deep/inner.cpp:2",), reject=("chain.cpp:2",))

    print("cpp_scope_smoke: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
