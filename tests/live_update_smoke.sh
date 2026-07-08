#!/bin/sh
set -eu

ctx_bin=${1:?missing ctx binary path}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/ctx-live-test.XXXXXX")
port=$((19000 + ($$ % 1000)))
pid=""

cleanup() {
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
}
trap cleanup EXIT INT TERM

cat > "$tmpdir/live.c" <<'SRC'
int ctx_live_alpha(void) {
    return 1;
}
SRC

"$ctx_bin" --no-gui --project "$tmpdir" --api-port "$port" > "$tmpdir/ctx.log" 2>&1 &
pid=$!

wait_for() {
    name=$1
    cmd=$2
    i=0
    while [ "$i" -lt 120 ]; do
        if sh -c "$cmd" >/dev/null 2>&1; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.1
    done
    echo "timed out waiting for $name" >&2
    cat "$tmpdir/ctx.log" >&2 || true
    return 1
}

wait_for "api health" "curl -fsS http://127.0.0.1:$port/health"
wait_for "initial index readiness" "curl -fsS http://127.0.0.1:$port/health | grep '\"ready\":true'"
curl -fsS "http://127.0.0.1:$port/health" | grep '"watcher_running":true' >/dev/null
curl -fsS "http://127.0.0.1:$port/status" | grep '"graph_generation"' >/dev/null

cat > "$tmpdir/live.c" <<'SRC'
int ctx_live_alpha(void) {
    return 1;
}

int ctx_live_beta(void) {
    return ctx_live_alpha();
}
SRC

wait_for "new symbol" "curl -fsS 'http://127.0.0.1:$port/context/symbol?name=ctx_live_beta' | grep 'fn       ctx_live_beta'"
curl -fsS "http://127.0.0.1:$port/context?task=fix+ctx_live_beta" > "$tmpdir/context.txt"
grep '^CTX_PACKET' "$tmpdir/context.txt" >/dev/null
grep '^ACCOUNTING' "$tmpdir/context.txt" >/dev/null
grep 'expand:source:' "$tmpdir/context.txt" >/dev/null
grep 'expand:entrypoints:' "$tmpdir/context.txt" >/dev/null
handle=$(sed -n 's/.*\(expand:source:[0-9][0-9]*\).*/\1/p' "$tmpdir/context.txt" | head -1)
test -n "$handle"
curl -fsS "http://127.0.0.1:$port/context/expand?handle=$handle" | grep 'SOURCE .*live.c' >/dev/null
curl -fsS "http://127.0.0.1:$port/context/expand?handle=expand:entrypoints:$tmpdir/live.c" > "$tmpdir/entrypoints.txt"
grep '^DETAIL: entrypoints-only' "$tmpdir/entrypoints.txt" >/dev/null
grep 'ctx_live_beta' "$tmpdir/entrypoints.txt" >/dev/null
curl -fsS "http://127.0.0.1:$port/context/expand?handle=expand:file:$tmpdir/live.c" > "$tmpdir/file-expand.txt"
grep '^DETAIL: compact-file-map' "$tmpdir/file-expand.txt" >/dev/null
grep '^ENTRYPOINTS' "$tmpdir/file-expand.txt" >/dev/null
grep 'symbols_total' "$tmpdir/file-expand.txt" >/dev/null
curl -fsS "http://127.0.0.1:$port/context/symbol?name=ctx_live_beta&detail=full" | grep '^CODEBASE:' >/dev/null
wait_for "stats freshness" "curl -fsS http://127.0.0.1:$port/stats | grep '\"watch_count\"'"

cat > "$tmpdir/live.c" <<'SRC'
int ctx_live_alpha(void) {
    return 2;
}
SRC

wait_for "removed symbol" "! curl -fsS 'http://127.0.0.1:$port/context/symbol?name=ctx_live_beta' | grep 'fn       ctx_live_beta'"
