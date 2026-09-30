#!/bin/sh
# Live file-watcher behaviour: every change class must leave the index equal
# to what a full scan would produce, and single changes / write bursts must
# produce exactly one graph update. Runs on Linux (inotify) and macOS
# (FSEvents); asserts through the HTTP API so it works in any build type.
set -eu

ctx_bin=${1:?missing ctx binary path}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/ctx-watch-test.XXXXXX")
proj="$tmpdir/proj"
outside="$tmpdir/outside"
port=$((20000 + ($$ % 1000)))
api="http://127.0.0.1:$port"
pid=""

cleanup() {
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $1" >&2
    cat "$tmpdir/ctx.log" >&2 || true
    exit 1
}

wait_for() {
    name=$1
    cmd=$2
    i=0
    while [ "$i" -lt 150 ]; do
        if eval "$cmd" >/dev/null 2>&1; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.1
    done
    fail "timed out waiting for $name"
}

# Prints the current graph generation (one increment per graph update).
generation() {
    curl -fsS "$api/status" | sed -n 's/.*"graph_generation":\([0-9]*\).*/\1/p'
}

# Succeeds when symbol $1 is indexed; with $2, only when it is in that file.
has_symbol() {
    curl -fsS "$api/context/symbol?name=$1" | grep -q "^- fn  *$1(.*)  ${2:-}"
}

# Waits until no graph update has happened for 1.5 s (debounce + job run).
settle() {
    last=$(generation)
    while :; do
        sleep 1.5
        now=$(generation)
        [ "$now" = "$last" ] && return 0
        last=$now
    done
}

expect_updates() {
    label=$1
    before=$2
    max=$3
    settle
    delta=$(( $(generation) - before ))
    [ "$delta" -ge 1 ] && [ "$delta" -le "$max" ] ||
        fail "$label: expected 1..$max graph updates, got $delta"
}

expect_no_updates() {
    label=$1
    before=$2
    settle
    delta=$(( $(generation) - before ))
    [ "$delta" -eq 0 ] || fail "$label: expected no graph update, got $delta"
}

mkdir -p "$proj/sub" "$outside/moved"
printf 'int w_alpha(void) { return 1; }\n' > "$proj/a.c"
printf 'int w_beta(void) { return 2; }\n'  > "$proj/b.c"
printf 'int w_gamma(void) { return 3; }\n' > "$proj/sub/c.c"
printf 'int w_moved_in(void) { return 4; }\n' > "$outside/moved/m.c"

HOME="$tmpdir/home" "$ctx_bin" --no-gui --project "$proj" --api-port "$port" \
    > "$tmpdir/ctx.log" 2>&1 &
pid=$!

wait_for "initial index" "curl -fsS $api/status | grep '\"ready\":true'"
curl -fsS "$api/status" | grep -q '"watcher_running":true' || fail "watcher not running"
has_symbol w_gamma sub/c.c || fail "initial index missing w_gamma"
settle

# Single edit -> exactly one update.
g=$(generation)
printf 'int w_append(void) { return 5; }\n' >> "$proj/a.c"
wait_for "appended symbol" "has_symbol w_append"
expect_updates "single append" "$g" 1

# Write burst -> collapsed (debounce may split a slow burst once).
g=$(generation)
i=0
while [ "$i" -lt 20 ]; do
    printf 'int w_burst_%d(void) { return %d; }\n' "$i" "$i" >> "$proj/b.c"
    i=$((i + 1))
done
wait_for "burst symbols" "has_symbol w_burst_19"
expect_updates "20-write burst" "$g" 2

# Permission changes and reads leave content and mtime untouched -> no
# update. (touch changes mtime, which is treated as a possible change.)
g=$(generation)
chmod 600 "$proj/a.c"
cat "$proj/a.c" > /dev/null
expect_no_updates "chmod/read" "$g"

# Atomic save (write temp + rename over) and a later in-place edit.
printf 'int w_atomic(void) { return 6; }\n' > "$proj/.a.c.tmp"
mv "$proj/.a.c.tmp" "$proj/a.c"
wait_for "atomic save" "has_symbol w_atomic && ! has_symbol w_alpha"
printf 'int w_after_atomic(void) { return 7; }\n' >> "$proj/a.c"
wait_for "edit after atomic save" "has_symbol w_after_atomic"

# Excluded locations are never indexed.
mkdir -p "$proj/build" "$proj/.hidden" "$proj/node_modules/pkg"
printf 'int w_in_build(void) { return 0; }\n'  > "$proj/build/gen.c"
printf 'int w_in_hidden(void) { return 0; }\n' > "$proj/.hidden/h.c"
printf 'int w_in_deps(void) { return 0; }\n'   > "$proj/node_modules/pkg/d.c"
printf 'int w_hidden_file(void) { return 0; }\n' > "$proj/.dot.c"
settle
for s in w_in_build w_in_hidden w_in_deps w_hidden_file; do
    has_symbol "$s" && fail "excluded symbol $s was indexed"
done

# New nested directory with a file created immediately.
mkdir -p "$proj/d1/d2" && printf 'int w_deep(void) { return 8; }\n' > "$proj/d1/d2/deep.c"
wait_for "file in new nested dir" "has_symbol w_deep d1/d2/deep.c"
printf 'int w_deep_edit(void) { return 9; }\n' >> "$proj/d1/d2/deep.c"
wait_for "edit in new nested dir" "has_symbol w_deep_edit d1/d2/deep.c"

# Directory moved into the tree, renamed within it, then edited.
mv "$outside/moved" "$proj/moved"
wait_for "dir moved in" "has_symbol w_moved_in moved/m.c"
mv "$proj/moved" "$proj/renamed"
wait_for "dir renamed" "has_symbol w_moved_in renamed/m.c && ! has_symbol w_moved_in moved/m.c"
printf 'int w_after_rename(void) { return 10; }\n' >> "$proj/renamed/m.c"
wait_for "edit in renamed dir" "has_symbol w_after_rename renamed/m.c"

# Directory moved out of the tree, then edited there.
mv "$proj/renamed" "$outside/gone"
wait_for "dir moved out" "! has_symbol w_moved_in"
printf 'int w_outside(void) { return 11; }\n' >> "$outside/gone/m.c"
settle
has_symbol w_outside && fail "edit outside the tree was indexed"

# File moved out, file deleted, file truncated, directory removed.
mv "$proj/b.c" "$outside/b.c"
wait_for "file moved out" "! has_symbol w_beta"
rm "$proj/sub/c.c"
wait_for "file deleted" "! has_symbol w_gamma"
: > "$proj/a.c"
wait_for "file truncated" "! has_symbol w_atomic"
rm -rf "$proj/d1"
wait_for "dir removed" "! has_symbol w_deep"

# A file created after all of the above is still picked up.
printf 'int w_final(void) { return 12; }\n' > "$proj/sub/final.c"
wait_for "final new file" "has_symbol w_final sub/final.c"

echo "watcher_smoke: ok"
