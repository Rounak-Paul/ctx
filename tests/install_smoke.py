#!/usr/bin/env python3
import json
import pathlib
import subprocess
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    if len(sys.argv) != 2:
        print("usage: install_smoke.py /path/to/ctx", file=sys.stderr)
        return 2

    ctx_bin = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="ctx-install-smoke.") as tmp:
        project = pathlib.Path(tmp) / "project"
        project.mkdir()
        resolved_project = project.resolve()
        (project / "sample.c").write_text("int main(void) { return 0; }\n")
        (project / ".codex").mkdir()
        (project / ".codex" / "config.toml").write_text(
            '[mcp_servers.ctx]\n'
            'command = "/old/ctx"\n'
            'args = ["--mcp", "--project", "/old/project"]\n'
            '\n'
            '[mcp_servers.keep]\n'
            'command = "keep"\n'
        )
        (project / ".mcp.json").write_text(json.dumps({
            "mcpServers": {
                "ctx": {"command": "/old/ctx", "args": []},
                "keep": {"command": "keep", "args": []},
            }
        }))
        (project / "opencode.json").write_text(json.dumps({
            "$schema": "https://opencode.ai/config.json",
            "instructions": [".ctx/ctx-agent-instructions.md"],
            "mcp": {
                "ctx": {"type": "local", "command": ["/old/ctx"]},
                "keep": {"type": "local", "command": ["keep"]},
            },
        }))

        cmd = [str(ctx_bin), "--install", "--project", str(project), "--clients", "all"]
        first = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=20)
        require(first.returncode == 0, first.stderr)
        second = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=20)
        require(second.returncode == 0, second.stderr)

        codex = (project / ".codex" / "config.toml").read_text()
        require(codex.count("[mcp_servers.ctx]") == 1, "codex config duplicated ctx server")
        require("[mcp_servers.keep]" in codex, "codex config dropped unrelated server")
        require("/old/ctx" not in codex, "codex config kept stale ctx server")
        require(f'"{ctx_bin}"' in codex, "codex config missing ctx binary")
        require(f'"{resolved_project}"' in codex, "codex config missing project path")
        codex_skill = (project / ".codex" / "skills" / "ctx" / "SKILL.md").read_text()
        require("get_context" in codex_skill, "codex skill missing retrieval policy")

        claude = json.loads((project / ".mcp.json").read_text())
        require(claude["mcpServers"]["ctx"]["command"] == str(ctx_bin), "claude command mismatch")
        require(claude["mcpServers"]["ctx"]["args"] == ["--mcp", "--project", str(resolved_project)], "claude args mismatch")
        require("keep" in claude["mcpServers"], "claude config dropped unrelated server")
        require("/old/ctx" not in json.dumps(claude["mcpServers"]["ctx"]), "claude config kept stale ctx server")

        opencode = json.loads((project / "opencode.json").read_text())
        ctx_mcp = opencode["mcp"]["ctx"]
        require(ctx_mcp["type"] == "local", "opencode ctx server is not local")
        require(ctx_mcp["command"] == [str(ctx_bin), "--mcp", "--project", str(resolved_project)], "opencode command mismatch")
        require("keep" in opencode["mcp"], "opencode config dropped unrelated server")
        require(opencode["instructions"].count(".ctx/ctx-agent-instructions.md") == 1, "opencode instructions duplicated")
        require(".ctx/ctx-agent-instructions.md" in opencode["instructions"], "opencode instructions missing")

        agents = (project / "AGENTS.md").read_text()
        claude_md = (project / ".claude" / "CLAUDE.md").read_text()
        claude_settings = json.loads((project / ".claude" / "settings.json").read_text())
        claude_skill = (project / ".claude" / "skills" / "ctx" / "SKILL.md").read_text()
        claude_rule = (project / ".claude" / "rules" / "ctx.md").read_text()
        shared = (project / ".ctx" / "ctx-agent-instructions.md").read_text()
        require(agents.count("BEGIN ctx managed") == 1, "AGENTS block duplicated")
        require(claude_md.count("BEGIN ctx managed") == 1, "CLAUDE block duplicated")
        require(claude_settings["$schema"] == "https://json.schemastore.org/claude-code-settings.json", "claude settings schema missing")
        require("get_context" in claude_skill, "claude skill missing retrieval policy")
        require("get_status" in claude_rule, "claude rule missing retrieval policy")
        require(not (project / "CLAUDE.md").exists(), "installer wrote root CLAUDE.md instead of .claude/CLAUDE.md")
        require("expand:lines" in shared, "shared instructions missing selective expansion policy")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
