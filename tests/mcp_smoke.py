#!/usr/bin/env python3
import json
import os
import subprocess
import sys
import tempfile
import time


def read_frame(proc):
    headers = {}
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("MCP server closed stdout")
        if line in (b"\r\n", b"\n"):
            break
        key, _, value = line.decode("ascii", "replace").partition(":")
        headers[key.lower()] = value.strip()
    length = int(headers.get("content-length", "0"))
    if length <= 0:
        raise RuntimeError("missing Content-Length in MCP response")
    payload = proc.stdout.read(length)
    return json.loads(payload.decode("utf-8"))


def send_frame(proc, msg):
    payload = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    proc.stdin.write(f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii"))
    proc.stdin.write(payload)
    proc.stdin.flush()


def call(proc, request_id, method, params=None):
    msg = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        msg["params"] = params
    send_frame(proc, msg)
    response = read_frame(proc)
    if "error" in response:
        raise RuntimeError(f"{method} failed: {response['error']}")
    return response


def text_content(response):
    content = response["result"]["content"]
    return "".join(item.get("text", "") for item in content if item.get("type") == "text")


def main():
    if len(sys.argv) != 2:
        print("usage: mcp_smoke.py /path/to/ctx", file=sys.stderr)
        return 2

    ctx_bin = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="ctx-mcp-smoke.") as tmp:
        project = os.path.join(tmp, "project")
        home = os.path.join(tmp, "home")
        os.makedirs(project)
        os.makedirs(home)
        source = os.path.join(project, "live.c")
        with open(source, "w", encoding="utf-8") as f:
            f.write(
                "int ctx_live_alpha(void) {\n"
                "    return 1;\n"
                "}\n\n"
                "int ctx_live_beta(void) {\n"
                "    return ctx_live_alpha();\n"
                "}\n"
            )

        env = dict(os.environ)
        env["HOME"] = home
        proc = subprocess.Popen(
            [ctx_bin, "--mcp", "--project", project],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        try:
            init = call(proc, 1, "initialize", {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "ctx-mcp-smoke", "version": "1"},
            })
            assert init["result"]["serverInfo"]["name"] == "ctx"

            tools = call(proc, 2, "tools/list")
            names = {tool["name"] for tool in tools["result"]["tools"]}
            required = {"get_context", "get_symbol", "get_file", "expand_context", "get_stats", "get_status"}
            missing = required - names
            if missing:
                raise AssertionError(f"missing tools: {sorted(missing)}")

            context = call(proc, 3, "tools/call", {
                "name": "get_context",
                "arguments": {"task": "explain ctx_live_beta"},
            })
            packet = text_content(context)
            assert "CTX_PACKET" in packet
            assert "ctx_live_beta" in packet
            assert "expand:entrypoints:live.c" in packet or "expand:file:live.c" in packet

            entrypoints = call(proc, 4, "tools/call", {
                "name": "expand_context",
                "arguments": {"handle": "expand:entrypoints:live.c"},
            })
            expanded = text_content(entrypoints)
            assert "DETAIL: entrypoints-only" in expanded
            assert "ctx_live_beta" in expanded

            source_id = None
            for token in expanded.split():
                if token.startswith("expand:source:"):
                    source_id = token.rsplit(":", 1)[-1]
                    break
            if not source_id:
                raise AssertionError("entrypoints expansion did not expose source handle")

            line_range = call(proc, 5, "tools/call", {
                "name": "expand_context",
                "arguments": {"handle": f"expand:lines:{source_id}:1-999"},
            })
            lines = text_content(line_range)
            assert "SOURCE live.c:" in lines
            assert "ctx_live" in lines

            status = call(proc, 6, "tools/call", {
                "name": "get_status",
                "arguments": {},
            })
            status_text = text_content(status)
            status_json = json.loads(status_text)
            assert status_json["ready"] is True
            assert status_json["watcher_running"] is True
        finally:
            proc.stdin.close()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)

        return 0


if __name__ == "__main__":
    raise SystemExit(main())
