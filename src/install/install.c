#include "install.h"
#include "../log/log.h"
#include "../../vendors/cjson/cJSON.h"

#include <ctype.h>

#define CTX_INSTALL_MARK_BEGIN "<!-- BEGIN ctx managed -->"
#define CTX_INSTALL_MARK_END   "<!-- END ctx managed -->"
#define CTX_TOML_MARK_BEGIN    "# BEGIN ctx managed"
#define CTX_TOML_MARK_END      "# END ctx managed"

typedef struct {
    char project[PATH_MAX];
    char bin[PATH_MAX];
    const char *clients;
} CtxInstallPlan;

/*
 * Formats text into a fixed buffer.
 *
 * dst: Destination buffer.
 * dst_cap: Destination buffer size in bytes.
 * fmt: printf-style format string.
 */
static bool format_checked(char *dst, size_t dst_cap, const char *fmt, ...)
{
    if (!dst || dst_cap == 0 || !fmt) return false;

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(dst, dst_cap, fmt, args);
    va_end(args);

    return written >= 0 && (size_t)written < dst_cap;
}

/*
 * Duplicates a NUL-terminated string.
 *
 * text: Text to copy.
 */
static char *string_dup(const char *text)
{
    size_t len = text ? strlen(text) : 0;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    if (len) memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

/*
 * Returns true when the selected client list contains the requested client.
 *
 * clients: Comma-separated client selector.
 * name: Lowercase client name to check.
 */
static bool wants_client(const char *clients, const char *name)
{
    if (!clients || !clients[0] || !strcmp(clients, "all")) return true;

    char buf[128];
    if (!format_checked(buf, sizeof(buf), "%s", clients)) return false;
    for (char *p = buf; *p; ++p) *p = (char)tolower((unsigned char)*p);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (!strcmp(tok, name)) return true;
    }
    return false;
}

/*
 * Validates the comma-separated client selector.
 *
 * clients: Comma-separated client selector.
 */
static bool validate_clients(const char *clients)
{
    if (!clients || !clients[0] || !strcmp(clients, "all")) return true;

    char buf[128];
    if (!format_checked(buf, sizeof(buf), "%s", clients)) return false;
    for (char *p = buf; *p; ++p) *p = (char)tolower((unsigned char)*p);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (!tok[0]) return false;
        if (strcmp(tok, "codex") && strcmp(tok, "claude") && strcmp(tok, "opencode"))
            return false;
    }
    return true;
}

/*
 * Copies a path into dst as an absolute path when the source exists.
 *
 * src: Source path.
 * dst: Destination buffer.
 * dst_cap: Destination buffer size in bytes.
 */
static bool resolve_existing_path(const char *src, char *dst, size_t dst_cap)
{
    if (!src || !src[0] || !dst || dst_cap == 0) return false;
    char resolved[PATH_MAX];
    if (!realpath(src, resolved)) return false;
    return format_checked(dst, dst_cap, "%s", resolved);
}

/*
 * Resolves argv[0] into an executable path suitable for MCP client configs.
 *
 * argv0: Executable name or path.
 * dst: Destination buffer.
 * dst_cap: Destination buffer size in bytes.
 */
static bool resolve_executable_path(const char *argv0, char *dst, size_t dst_cap)
{
    if (resolve_existing_path(argv0, dst, dst_cap)) return true;

    if (argv0 && strchr(argv0, '/')) {
        return format_checked(dst, dst_cap, "%s", argv0);
    }

    const char *path_env = getenv("PATH");
    if (!argv0 || !argv0[0] || !path_env) return false;

    char paths[8192];
    if (!format_checked(paths, sizeof(paths), "%s", path_env)) return false;
    char *save = NULL;
    for (char *dir = strtok_r(paths, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char candidate[PATH_MAX];
        if (!format_checked(candidate, sizeof(candidate), "%s/%s", dir[0] ? dir : ".", argv0))
            continue;
        if (access(candidate, X_OK) == 0)
            return resolve_existing_path(candidate, dst, dst_cap);
    }
    return false;
}

/*
 * Creates a directory and all missing parents.
 *
 * path: Directory path to create.
 */
static bool mkdir_p(const char *path)
{
    if (!path || !path[0]) return false;

    char tmp[PATH_MAX];
    if (!format_checked(tmp, sizeof(tmp), "%s", path)) return false;
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

/*
 * Reads a complete file into a NUL-terminated heap buffer.
 *
 * path: File path to read.
 */
static char *read_text_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);

    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/*
 * Writes text atomically enough for small config files by using a temp file and rename.
 *
 * path: Destination file path.
 * text: NUL-terminated text to write.
 */
static bool write_text_file(const char *path, const char *text)
{
    char dir[PATH_MAX];
    if (!format_checked(dir, sizeof(dir), "%s", path)) return false;
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (!mkdir_p(dir)) return false;
    }

    char tmp[PATH_MAX];
    if (!format_checked(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()))
        return false;
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    size_t len = text ? strlen(text) : 0;
    bool ok = fwrite(text ? text : "", 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

/*
 * Builds file text with a single managed block.
 *
 * old: Existing file text, or NULL for a new file.
 * begin: Managed block begin marker.
 * end: Managed block end marker.
 * block: Complete replacement block including markers.
 */
static char *build_marked_block_text(const char *old, const char *begin, const char *end, const char *block)
{
    size_t old_len = old ? strlen(old) : 0;

    const char *prefix_end = old ? old + old_len : NULL;
    const char *suffix_start = NULL;
    char *begin_pos = old ? strstr(old, begin) : NULL;
    if (begin_pos) {
        char *end_pos = strstr(begin_pos, end);
        if (end_pos) {
            prefix_end = begin_pos;
            suffix_start = end_pos + strlen(end);
            while (*suffix_start == '\r' || *suffix_start == '\n') suffix_start++;
        }
    }

    size_t prefix_len = prefix_end && old ? (size_t)(prefix_end - old) : 0;
    size_t suffix_len = suffix_start ? strlen(suffix_start) : 0;
    size_t block_len = strlen(block);
    size_t need_sep = prefix_len > 0 && old[prefix_len - 1] != '\n' ? 1 : 0;
    size_t total = prefix_len + need_sep + block_len + 2 + suffix_len + 1;

    char *out = calloc(1, total);
    if (!out) return NULL;
    if (prefix_len) strncat(out, old, prefix_len);
    if (need_sep) strcat(out, "\n");
    strcat(out, block);
    if (suffix_len) {
        if (out[strlen(out) - 1] != '\n') strcat(out, "\n");
        strcat(out, suffix_start);
    }

    return out;
}

/*
 * Appends or replaces a marked block in a text file.
 *
 * path: File path to update.
 * begin: Managed block begin marker.
 * end: Managed block end marker.
 * block: Complete replacement block including markers.
 */
static bool write_marked_block(const char *path, const char *begin, const char *end, const char *block)
{
    char *old = read_text_file(path);
    char *out = build_marked_block_text(old, begin, end, block);
    if (!out) {
        free(old);
        return false;
    }

    bool ok = write_text_file(path, out);
    free(out);
    free(old);
    return ok;
}

/*
 * Removes an unmarked TOML table from existing config text.
 *
 * text: Existing TOML text.
 * table: Exact table heading, including brackets.
 */
static char *remove_toml_table(const char *text, const char *table)
{
    if (!text) return calloc(1, 1);

    size_t cap = strlen(text) + 1;
    char *out = calloc(1, cap);
    if (!out) return NULL;

    bool skipping = false;
    const char *line = text;
    while (*line) {
        const char *next = strchr(line, '\n');
        size_t line_len = next ? (size_t)(next - line + 1) : strlen(line);

        const char *trim = line;
        while (*trim == ' ' || *trim == '\t') trim++;

        if (!skipping && !strncmp(trim, table, strlen(table))) {
            const char *after = trim + strlen(table);
            if (*after == '\0' || *after == '\r' || *after == '\n' || *after == ' ' || *after == '\t') {
                skipping = true;
                line += line_len;
                continue;
            }
        }

        if (skipping && *trim == '[') skipping = false;
        if (!skipping) strncat(out, line, line_len);
        line += line_len;
    }

    return out;
}

/*
 * Escapes a C string for TOML basic string syntax.
 *
 * text: Text to escape.
 */
static char *toml_escape(const char *text)
{
    size_t extra = 0;
    for (const char *p = text; p && *p; ++p)
        if (*p == '\\' || *p == '"') extra++;

    size_t len = text ? strlen(text) : 0;
    char *out = malloc(len + extra + 1);
    if (!out) return NULL;

    char *w = out;
    for (const char *p = text; p && *p; ++p) {
        if (*p == '\\' || *p == '"') *w++ = '\\';
        *w++ = *p;
    }
    *w = '\0';
    return out;
}

/*
 * Writes the shared agent instructions used by all supported clients.
 *
 * plan: Valid install plan.
 */
static bool install_shared_instructions(const CtxInstallPlan *plan)
{
    char path[PATH_MAX];
    if (!format_checked(path, sizeof(path), "%s/.ctx/ctx-agent-instructions.md", plan->project))
        return false;

    const char *text =
        "# ctx Agent Instructions\n\n"
        "Use ctx as the first codebase-retrieval step for agentic coding tasks in this project.\n\n"
        "- Call `get_status` first when freshness matters; wait for `ready: true` before relying on retrieval.\n"
        "- Call `get_context` once with a concrete task before scanning files directly.\n"
        "- Treat `CTX_PACKET` as the working map: use `ANSWER_MAP`, `EDIT_TARGETS`, `RELEVANT_FILES`, and `SYMBOL_CARDS` before expanding.\n"
        "- Prefer `expand:entrypoints:<path>` for file-level API shape.\n"
        "- Prefer `expand:lines:<id>:<start>-<end>` for exact edit context.\n"
        "- Use `expand:source:<id>` only when the full symbol body is required.\n"
        "- Avoid `detail=full` unless compact context is insufficient and the extra source text is justified.\n"
        "- Stop expanding when the packet answers the question; ctx saves credits only when handles are expanded selectively.\n";

    return write_text_file(path, text);
}

/*
 * Writes the Codex project-local skill used when Codex supports skill discovery.
 *
 * plan: Valid install plan.
 */
static bool install_codex_skill(const CtxInstallPlan *plan)
{
    char path[PATH_MAX];
    if (!format_checked(path, sizeof(path), "%s/.codex/skills/ctx/SKILL.md", plan->project))
        return false;

    const char *text =
        "---\n"
        "name: \"ctx\"\n"
        "description: \"Use for coding tasks in this repository. Retrieves compact, fresh codebase context from the project-local ctx MCP server before broad file reads.\"\n"
        "---\n"
        "\n"
        "# ctx\n"
        "\n"
        "Use the `ctx` MCP server as the first codebase-retrieval step for agentic coding tasks in this repository.\n"
        "\n"
        "1. Call `get_status` first when freshness matters; wait for `ready: true` before relying on retrieval.\n"
        "2. Call `get_context` once with a concrete task before scanning files directly.\n"
        "3. Treat `CTX_PACKET` as the working map: use `ANSWER_MAP`, `EDIT_TARGETS`, `RELEVANT_FILES`, and `SYMBOL_CARDS` before expanding.\n"
        "4. Prefer `expand:entrypoints:<path>` for file-level API shape.\n"
        "5. Prefer `expand:lines:<id>:<start>-<end>` for exact edit context.\n"
        "6. Use `expand:source:<id>` only when the full symbol body is required.\n"
        "7. Avoid `detail=full` unless compact context is insufficient and the extra source text is justified.\n"
        "8. Stop expanding when the packet answers the question.\n";

    return write_text_file(path, text);
}

/*
 * Writes Codex project MCP config, skill, and AGENTS.md usage instructions.
 *
 * plan: Valid install plan.
 */
static bool install_codex(const CtxInstallPlan *plan)
{
    char *bin = toml_escape(plan->bin);
    char *project = toml_escape(plan->project);
    if (!bin || !project) {
        free(bin);
        free(project);
        return false;
    }

    char config_path[PATH_MAX];
    if (!format_checked(config_path, sizeof(config_path), "%s/.codex/config.toml", plan->project)) {
        free(bin);
        free(project);
        return false;
    }

    char block[PATH_MAX * 2];
    if (!format_checked(block, sizeof(block),
        "%s\n"
        "[mcp_servers.ctx]\n"
        "command = \"%s\"\n"
        "args = [\"--mcp\", \"--project\", \"%s\"]\n"
        "startup_timeout_sec = 120\n"
        "%s\n",
        CTX_TOML_MARK_BEGIN, bin, project, CTX_TOML_MARK_END)) {
        free(bin);
        free(project);
        return false;
    }

    free(bin);
    free(project);

    char *old = read_text_file(config_path);
    char *clean = old && strstr(old, CTX_TOML_MARK_BEGIN) && strstr(old, CTX_TOML_MARK_END)
        ? string_dup(old)
        : remove_toml_table(old, "[mcp_servers.ctx]");
    char *merged = clean ? build_marked_block_text(clean, CTX_TOML_MARK_BEGIN, CTX_TOML_MARK_END, block) : NULL;
    free(old);
    free(clean);
    if (!merged) return false;

    bool ok = write_text_file(config_path, merged);
    free(merged);
    if (!ok)
        return false;

    if (!install_codex_skill(plan))
        return false;

    char agents_path[PATH_MAX];
    if (!format_checked(agents_path, sizeof(agents_path), "%s/AGENTS.md", plan->project))
        return false;
    const char *agents_block =
        CTX_INSTALL_MARK_BEGIN "\n"
        "## ctx MCP\n"
        "Use the `ctx` MCP server for codebase retrieval before broad file reads. Start with `get_status`, then `get_context` for the current task. Expand only the handles needed for the next edit, preferring `expand:entrypoints` and `expand:lines`; avoid `detail=full` and `expand:source` unless the full body is required.\n"
        "\n"
        "See `.ctx/ctx-agent-instructions.md` for the full ctx usage policy.\n"
        CTX_INSTALL_MARK_END "\n";
    return write_marked_block(agents_path, CTX_INSTALL_MARK_BEGIN, CTX_INSTALL_MARK_END, agents_block);
}

/*
 * Builds the standard stdio MCP server object for JSON-based clients.
 *
 * plan: Valid install plan.
 * opencode_shape: true to use OpenCode's local command array shape.
 */
static cJSON *build_json_mcp_server(const CtxInstallPlan *plan, bool opencode_shape)
{
    cJSON *server = cJSON_CreateObject();
    if (!server) return NULL;
    if (opencode_shape) {
        cJSON_AddStringToObject(server, "type", "local");
        cJSON_AddBoolToObject(server, "enabled", true);
        cJSON_AddNumberToObject(server, "timeout", 120000);
    }

    cJSON *cmd = cJSON_CreateArray();
    if (!cmd) {
        cJSON_Delete(server);
        return NULL;
    }

    if (opencode_shape) {
        cJSON_AddItemToArray(cmd, cJSON_CreateString(plan->bin));
        cJSON_AddItemToArray(cmd, cJSON_CreateString("--mcp"));
        cJSON_AddItemToArray(cmd, cJSON_CreateString("--project"));
        cJSON_AddItemToArray(cmd, cJSON_CreateString(plan->project));
        cJSON_AddItemToObject(server, "command", cmd);
    } else {
        cJSON_AddStringToObject(server, "command", plan->bin);
        cJSON_AddItemToArray(cmd, cJSON_CreateString("--mcp"));
        cJSON_AddItemToArray(cmd, cJSON_CreateString("--project"));
        cJSON_AddItemToArray(cmd, cJSON_CreateString(plan->project));
        cJSON_AddItemToObject(server, "args", cmd);
    }
    return server;
}

/*
 * Loads a JSON object from path or returns a new object when the file is absent.
 *
 * path: JSON file path.
 */
static cJSON *load_or_create_json_object(const char *path)
{
    char *text = read_text_file(path);
    if (!text) return cJSON_CreateObject();

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/*
 * Writes JSON with stable formatting.
 *
 * path: Destination JSON path.
 * root: JSON object to print.
 */
static bool write_json_file(const char *path, cJSON *root)
{
    char *text = cJSON_Print(root);
    if (!text) return false;
    bool ok = write_text_file(path, text);
    free(text);
    return ok;
}

/*
 * Writes Claude Code project settings metadata.
 *
 * plan: Valid install plan.
 */
static bool install_claude_settings(const CtxInstallPlan *plan)
{
    char path[PATH_MAX];
    if (!format_checked(path, sizeof(path), "%s/.claude/settings.json", plan->project))
        return false;

    cJSON *root = load_or_create_json_object(path);
    if (!root) {
        CTX_LOG_ERROR("Cannot merge %s because it is not valid JSON", path);
        return false;
    }

    if (!cJSON_GetObjectItemCaseSensitive(root, "$schema"))
        cJSON_AddStringToObject(root, "$schema", "https://json.schemastore.org/claude-code-settings.json");

    bool ok = write_json_file(path, root);
    cJSON_Delete(root);
    return ok;
}

/*
 * Writes Claude Code skill and rule files for project-local discovery.
 *
 * plan: Valid install plan.
 */
static bool install_claude_skill_and_rules(const CtxInstallPlan *plan)
{
    char skill_path[PATH_MAX];
    if (!format_checked(skill_path, sizeof(skill_path), "%s/.claude/skills/ctx/SKILL.md", plan->project))
        return false;

    const char *skill_text =
        "---\n"
        "name: ctx\n"
        "description: Use for coding tasks in this repository. Retrieves compact, fresh codebase context from the project-local ctx MCP server before broad file reads.\n"
        "---\n"
        "\n"
        "# ctx\n"
        "\n"
        "Use the `ctx` MCP server as the first retrieval step for coding tasks in this repository.\n"
        "\n"
        "1. Call `get_status` first when freshness matters; wait for `ready: true` before relying on retrieval.\n"
        "2. Call `get_context` once with a concrete task before scanning files directly.\n"
        "3. Use `CTX_PACKET` as the working map before expanding files.\n"
        "4. Prefer `expand:entrypoints:<path>` and `expand:lines:<id>:<start>-<end>`.\n"
        "5. Avoid `detail=full` and `expand:source` unless the full body is required.\n";

    if (!write_text_file(skill_path, skill_text))
        return false;

    char rule_path[PATH_MAX];
    if (!format_checked(rule_path, sizeof(rule_path), "%s/.claude/rules/ctx.md", plan->project))
        return false;

    const char *rule_text =
        "# ctx retrieval\n\n"
        "Use the project-local `ctx` MCP server before broad file reads on coding tasks. Start with `get_status`, then `get_context`; expand only the handles needed for the next edit.\n";

    return write_text_file(rule_path, rule_text);
}

/*
 * Writes Claude Code project MCP config, settings, skills, rules, and instructions.
 *
 * plan: Valid install plan.
 */
static bool install_claude(const CtxInstallPlan *plan)
{
    char path[PATH_MAX];
    if (!format_checked(path, sizeof(path), "%s/.mcp.json", plan->project))
        return false;

    cJSON *root = load_or_create_json_object(path);
    if (!root) {
        CTX_LOG_ERROR("Cannot merge %s because it is not valid JSON", path);
        return false;
    }

    cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
    if (!cJSON_IsObject(servers)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
        servers = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "mcpServers", servers);
    }

    cJSON_DeleteItemFromObjectCaseSensitive(servers, "ctx");
    cJSON *ctx_server = build_json_mcp_server(plan, false);
    if (!ctx_server) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_AddItemToObject(servers, "ctx", ctx_server);
    bool ok = write_json_file(path, root);
    cJSON_Delete(root);
    if (!ok) return false;

    if (!install_claude_settings(plan))
        return false;
    if (!install_claude_skill_and_rules(plan))
        return false;

    char claude_path[PATH_MAX];
    if (!format_checked(claude_path, sizeof(claude_path), "%s/.claude/CLAUDE.md", plan->project))
        return false;
    const char *claude_block =
        CTX_INSTALL_MARK_BEGIN "\n"
        "## ctx MCP\n"
        "Use the `ctx` MCP server before broad codebase reads. Start with `get_status`, then `get_context`; expand only needed handles, preferring `expand:entrypoints` and `expand:lines`. Avoid `detail=full` and full-source expansion unless necessary.\n"
        "\n"
        "See `.ctx/ctx-agent-instructions.md` for the full ctx usage policy.\n"
        CTX_INSTALL_MARK_END "\n";
    return write_marked_block(claude_path, CTX_INSTALL_MARK_BEGIN, CTX_INSTALL_MARK_END, claude_block);
}

/*
 * Writes OpenCode project config with ctx MCP and instruction-file registration.
 *
 * plan: Valid install plan.
 */
static bool install_opencode(const CtxInstallPlan *plan)
{
    char path[PATH_MAX];
    if (!format_checked(path, sizeof(path), "%s/opencode.json", plan->project))
        return false;

    cJSON *root = load_or_create_json_object(path);
    if (!root) {
        CTX_LOG_ERROR("Cannot merge %s because it is not valid JSON", path);
        return false;
    }

    if (!cJSON_GetObjectItemCaseSensitive(root, "$schema"))
        cJSON_AddStringToObject(root, "$schema", "https://opencode.ai/config.json");

    cJSON *mcp = cJSON_GetObjectItemCaseSensitive(root, "mcp");
    if (!cJSON_IsObject(mcp)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "mcp");
        mcp = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "mcp", mcp);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(mcp, "ctx");
    cJSON *ctx_server = build_json_mcp_server(plan, true);
    if (!ctx_server) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_AddItemToObject(mcp, "ctx", ctx_server);

    cJSON *instructions = cJSON_GetObjectItemCaseSensitive(root, "instructions");
    if (!cJSON_IsArray(instructions)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "instructions");
        instructions = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "instructions", instructions);
    }

    bool has_instruction = false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, instructions) {
        if (cJSON_IsString(item) && item->valuestring &&
            !strcmp(item->valuestring, ".ctx/ctx-agent-instructions.md")) {
            has_instruction = true;
            break;
        }
    }
    if (!has_instruction)
        cJSON_AddItemToArray(instructions, cJSON_CreateString(".ctx/ctx-agent-instructions.md"));

    bool ok = write_json_file(path, root);
    cJSON_Delete(root);
    return ok;
}

bool ctx_install_run(const CtxAppConfig *cfg, const char *argv0)
{
    if (!cfg) return false;

    CtxInstallPlan plan = {0};
    plan.clients = cfg->install_clients[0] ? cfg->install_clients : "all";
    if (!validate_clients(plan.clients)) {
        CTX_LOG_ERROR("Invalid --clients value '%s' (use all, codex, claude, opencode, or a comma list)", plan.clients);
        return false;
    }

    if (!resolve_existing_path(cfg->project_path, plan.project, sizeof(plan.project))) {
        CTX_LOG_ERROR("Project path does not exist: %s", cfg->project_path);
        return false;
    }
    if (!resolve_executable_path(argv0, plan.bin, sizeof(plan.bin))) {
        CTX_LOG_ERROR("Cannot resolve ctx executable path from %s", argv0 ? argv0 : "(null)");
        return false;
    }

    bool ok = install_shared_instructions(&plan);
    if (ok && wants_client(plan.clients, "codex")) ok = install_codex(&plan);
    if (ok && wants_client(plan.clients, "claude")) ok = install_claude(&plan);
    if (ok && wants_client(plan.clients, "opencode")) ok = install_opencode(&plan);

    if (!ok) return false;

    CTX_LOG_INFO("Installed ctx MCP integration for %s in %s", plan.clients, plan.project);
    CTX_LOG_INFO("MCP command: %s --mcp --project %s", plan.bin, plan.project);
    return true;
}
