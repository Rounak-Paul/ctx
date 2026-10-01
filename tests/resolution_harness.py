"""Shared harness for resolution smoke tests: writes a throwaway project,
runs `ctx --mcp` on it and exposes the navigation tools."""
import os
import subprocess
import tempfile
import time

from mcp_smoke import Client, require


class Project:
    """Context manager owning a temporary project and its ctx MCP server.

    files  Mapping of project-relative path to file content.
    """

    def __init__(self, ctx_bin, files, name):
        self.ctx_bin = ctx_bin
        self.files = files
        self.name = name
        self.tmp = None
        self.proc = None
        self.client = None
        self.root = None

    def __enter__(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=f"ctx-{self.name}.")
        self.root = os.path.join(self.tmp.name, "project")
        home = os.path.join(self.tmp.name, "home")
        os.makedirs(home)
        for rel, body in self.files.items():
            self.write(rel, body)
        env = dict(os.environ, HOME=home, CTX_MODELS="0")
        self.proc = subprocess.Popen(
            [self.ctx_bin, "--mcp", "--project", self.root],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
        )
        self.client = Client(self.proc)
        self.client.call("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": f"ctx-{self.name}", "version": "1"},
        })
        return self

    def __exit__(self, *exc):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        self.tmp.cleanup()
        return False

    def write(self, rel, body):
        """Writes (or rewrites) a project file."""
        path = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(body)

    def tool(self, name, arguments):
        return self.client.tool(name, arguments)

    def callees(self, symbol, expect=(), reject=()):
        """Asserts the resolved callee rows of symbol contain every expect
        location ("file:line") and no reject location."""
        text = self.tool("callees", {"symbol": symbol})
        targets = set()
        for row in text.splitlines():
            parts = row.split()
            if row.startswith("  L") and len(parts) > 2:
                path, _, lines = parts[2].rpartition(":")
                targets.add(f"{path}:{lines.split('-')[0]}")
        for item in expect:
            require(item in targets, f"{symbol}: expected {item} in:\n{text}")
        for item in reject:
            require(item not in targets, f"{symbol}: unexpected {item} in:\n{text}")
        return text

    def callers(self, symbol, expect=(), reject=()):
        """Asserts the callers of symbol mention every expect and no reject name."""
        text = self.tool("callers", {"symbol": symbol})
        for item in expect:
            require(item in text, f"callers of {symbol}: expected {item} in:\n{text}")
        for item in reject:
            require(item not in text, f"callers of {symbol}: unexpected {item} in:\n{text}")
        return text

    def wait_for_callee(self, symbol, item, timeout=15):
        """Polls callees of symbol until item appears (file-watcher reindex)."""
        deadline = time.time() + timeout
        while True:
            text = self.tool("callees", {"symbol": symbol})
            if item in text:
                return text
            require(time.time() < deadline, f"{symbol}: {item} never appeared:\n{text}")
            time.sleep(0.25)
