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
wait_for "stats freshness" "curl -fsS http://127.0.0.1:$port/stats | grep '\"watch_count\"'"

cat > "$tmpdir/live.c" <<'SRC'
int ctx_live_alpha(void) {
    return 2;
}
SRC

wait_for "removed symbol" "! curl -fsS 'http://127.0.0.1:$port/context/symbol?name=ctx_live_beta' | grep 'fn       ctx_live_beta'"
