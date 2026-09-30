#!/usr/bin/env python3
"""MCP smoke test: tool listing, every navigation tool, freshness after an
edit, and both stdio framings (newline-delimited and Content-Length)."""
import json
import os
import subprocess
import sys
import tempfile


def read_line_message(proc):
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError("MCP server closed stdout")
    return json.loads(line.decode("utf-8"))


def read_framed_message(proc):
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
    return json.loads(proc.stdout.read(length).decode("utf-8"))


def send(proc, msg, framed):
    payload = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    if framed:
        proc.stdin.write(f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii"))
        proc.stdin.write(payload)
    else:
        proc.stdin.write(payload + b"\n")
    proc.stdin.flush()


class Client:
    def __init__(self, proc):
        self.proc = proc
        self.next_id = 1

    def call(self, method, params=None, framed=False):
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        self.next_id += 1
        if params is not None:
            msg["params"] = params
        send(self.proc, msg, framed)
        response = read_framed_message(self.proc) if framed else read_line_message(self.proc)
        if "error" in response:
            raise RuntimeError(f"{method} failed: {response['error']}")
        return response

    def tool(self, name, arguments, expect_error=False):
        result = self.call("tools/call", {"name": name, "arguments": arguments})["result"]
        text = "".join(item.get("text", "") for item in result["content"] if item.get("type") == "text")
        if bool(result.get("isError")) != expect_error:
            raise AssertionError(f"{name} isError={result.get('isError')}: {text}")
        return text


def require(condition, message):
    if not condition:
        raise AssertionError(message)


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
                "/* Returns the base value. */\n"
                "int ctx_live_alpha(void) {\n"
                "    return 1;\n"
                "}\n\n"
                "int ctx_live_beta(void) {\n"
                "    return ctx_live_alpha();\n"
                "}\n"
            )

        env = dict(os.environ)
        env["HOME"] = home
        env["CTX_MODELS"] = "0"
        proc = subprocess.Popen(
            [ctx_bin, "--mcp", "--project", project],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
        )
        client = Client(proc)
        try:
            init = client.call("initialize", {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "ctx-mcp-smoke", "version": "1"},
            })
            require(init["result"]["serverInfo"]["name"] == "ctx", "unexpected server name")

            tools = client.call("tools/list", framed=True)
            names = {tool["name"] for tool in tools["result"]["tools"]}
            expected = {"search", "outline", "source", "callers", "callees", "impact", "status"}
            require(names == expected, f"tool set mismatch: {sorted(names)}")

            found = client.tool("search", {"query": "live alpha base value", "bodies": 0})
            require("ctx_live_alpha" in found and "live.c:2" in found, found)

            outline = client.tool("outline", {"path": "live.c"})
            require("L2-4" in outline and "ctx_live_beta" in outline, outline)

            body = client.tool("source", {"symbol": "ctx_live_alpha"})
            require("Returns the base value" in body and "return 1;" in body, body)

            callers = client.tool("callers", {"symbol": "ctx_live_alpha"})
            require("live.c:7" in callers and "ctx_live_beta" in callers, callers)

            callees = client.tool("callees", {"symbol": "ctx_live_beta"})
            require("ctx_live_alpha" in callees, callees)

            impact = client.tool("impact", {"symbol": "ctx_live_alpha"})
            require("direct call sites: 1" in impact, impact)

            missing = client.tool("source", {"symbol": "ctx_live_missing"}, expect_error=True)
            require("not found" in missing and "ctx_live_alpha" in missing, missing)

            with open(source, "w", encoding="utf-8") as f:
                f.write(
                    "int ctx_live_gamma(void) {\n"
                    "    return 3;\n"
                    "}\n\n"
                    "int ctx_live_alpha(void) {\n"
                    "    return ctx_live_gamma();\n"
                    "}\n\n"
                    "int ctx_live_beta(void) {\n"
                    "    return ctx_live_alpha();\n"
                    "}\n"
                )
            fresh = client.tool("source", {"symbol": "ctx_live_alpha"})
            require("live.c:5-7" in fresh and "ctx_live_gamma()" in fresh, fresh)
            callers = client.tool("callers", {"symbol": "ctx_live_alpha"})
            require("live.c:10" in callers, callers)
            gamma = client.tool("callers", {"symbol": "ctx_live_gamma"})
            require("live.c:6" in gamma, gamma)

            status = json.loads(client.tool("status", {}))
            require(status["status"] == "ready", status)
            require(status["models"]["reranker"]["state"] == "disabled", status)
        finally:
            proc.stdin.close()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
