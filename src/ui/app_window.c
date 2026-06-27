#ifdef CTX_HAS_CAUSALITY

#include "app_window.h"
#include "../log/log.h"
#include "../event/event.h"
#include "../indexer/indexer.h"
#include "../graph/graph.h"
#include "../retrieve/retrieve.h"
#include "../store/store.h"
#include "../jobs/jobs.h"
#include "force_graph.h"

#include <ctype.h>

/* ca_input_key_pressed() takes a raw GLFW key code; GLFW's headers are not on
 * this target's include path, so mirror the one constant we need. */
#define CTX_KEY_ENTER 257

/* ============================================================
   Palette — packed RGBA uint32 (R<<24 | G<<16 | B<<8 | A)
   ============================================================ */
#define C_BG0       0x090a0cffU  /* deepest bg            */
#define C_BG1       0x0e1013ffU  /* panel bg              */
#define C_BG2       0x161a1fffU  /* elevated surface      */
#define C_BG3       0x1e232bffU  /* hover / selected      */
#define C_BORDER    0x222933ffU  /* hairline border       */
#define C_ACCENT    0x7aa2f7ffU  /* blue accent           */
#define C_ACCENT2   0x9ece6affU  /* green ok              */
#define C_ACCENT3   0xe0af68ffU  /* amber warn            */
#define C_FG0       0xc0caf5ffU  /* primary text          */
#define C_FG1       0xa9b1d6ffU  /* secondary text        */
#define C_FG2       0x565f89ffU  /* muted text            */
#define C_FG3       0x2d334affU  /* very muted            */
#define C_NODE_FN   0x7aa2f730U  /* fn node header        */
#define C_NODE_STR  0x9ece6a30U  /* struct node header    */
#define C_NODE_MAC  0xe0af6830U  /* macro node header     */
#define C_WIRE      0x7aa2f788U  /* call edge wire        */

#define CTX_TAB_COUNT 5
#define LIST_PAGE_SIZE 80u
#define LIST_CELL_SMALL 64u
#define LIST_CELL_MEDIUM 192u
#define LIST_CELL_LARGE 320u

typedef struct {
    char name[LIST_CELL_MEDIUM];
    char kind[LIST_CELL_SMALL];
    char file[LIST_CELL_MEDIUM];
    char scope[LIST_CELL_MEDIUM];
    char line[LIST_CELL_SMALL];
} SymbolListRow;

typedef struct {
    char caller[LIST_CELL_MEDIUM];
    char caller_file[LIST_CELL_MEDIUM];
    char callee[LIST_CELL_MEDIUM];
    char callee_file[LIST_CELL_MEDIUM];
    char kind[LIST_CELL_SMALL];
} CallListRow;

typedef struct {
    char file[LIST_CELL_LARGE];
    char lang[LIST_CELL_SMALL];
    char symbols[LIST_CELL_SMALL];
    char size[LIST_CELL_SMALL];
    char errors[LIST_CELL_SMALL];
    char dir[LIST_CELL_LARGE];
} FileListRow;

typedef struct {
    int tab;
    uint64_t generation;
    uint32_t total;
    uint32_t row_count;
    bool ready;
    bool running;
    char query[160];
    uint32_t page;
} ListSnapshotMeta;

typedef struct {
    ListSnapshotMeta meta;
    union {
        SymbolListRow symbols[LIST_PAGE_SIZE];
        CallListRow calls[LIST_PAGE_SIZE];
        FileListRow files[LIST_PAGE_SIZE];
    } rows;
} ListSnapshot;

typedef struct {
    int tab;
    uint32_t page;
    uint64_t generation;
    char query[160];
} ListJobData;

typedef struct {
    uint64_t generation;
    char query[512];
} ContextJobData;

/* ============================================================
   State
   ============================================================ */
typedef struct {
    Ca_Instance  *inst;
    Ca_Window    *win;

    /* Navigation */
    Ca_Div       *tabs_div;      /* sticky tab strip, rebuilt when active tab changes */
    Ca_Div       *content_div;   /* stable panel body, rebuilt on active-tab data changes */
    volatile int  active_tab;    /* 0=Graph 1=Symbols 2=Calls 3=Context 4=Files */

    Ca_Progress  *prog_bar;

    /* Context retrieval playground */
    Ca_TextInput *ctx_query_input;
    char          ctx_query[512];
    char         *ctx_result;    /* heap-allocated structured context block; owned */
    uint64_t      ctx_generation;
    bool          ctx_running;
    char          list_query[CTX_TAB_COUNT][160];
    uint32_t      list_page[CTX_TAB_COUNT];
    uint64_t      list_generation[CTX_TAB_COUNT];
    ListSnapshot  list_snapshots[CTX_TAB_COUNT];

    /* Interactive graph viewport */
    CtxForceGraph *force_graph;
    float          mouse_x;
    float          mouse_y;
    bool           content_needs_rebuild;

    volatile bool graph_updated;
    volatile bool closing;
} AppState;

static AppState s;

#if defined(CTX_PLATFORM_WINDOWS)
static CRITICAL_SECTION s_ui_lock;
static void ui_lock(void) { EnterCriticalSection(&s_ui_lock); }
static void ui_unlock(void) { LeaveCriticalSection(&s_ui_lock); }
#else
static pthread_mutex_t s_ui_lock = PTHREAD_MUTEX_INITIALIZER;
static void ui_lock(void) { pthread_mutex_lock(&s_ui_lock); }
static void ui_unlock(void) { pthread_mutex_unlock(&s_ui_lock); }
#endif

static void build_tabs(Ca_Div *div, void *ud);
static void build_content(Ca_Div *div, void *ud);
static void request_list_refresh(int tab);
static void context_query_job(void *ud);

typedef struct {
    int tab;
} TabActionCtx;

typedef struct {
    int tab;
    int delta;
} PageActionCtx;

static TabActionCtx g_tab_actions[5] = {
    { .tab = 0 },
    { .tab = 1 },
    { .tab = 2 },
    { .tab = 3 },
    { .tab = 4 },
};

static PageActionCtx g_page_actions[] = {
    { .tab = 1, .delta = -1 }, { .tab = 1, .delta = 1 },
    { .tab = 2, .delta = -1 }, { .tab = 2, .delta = 1 },
    { .tab = 4, .delta = -1 }, { .tab = 4, .delta = 1 },
};

/* ============================================================
   CSS — flat, sharp (zero corner_radius in code, all via struct fields)
   ============================================================ */
static const char *g_css =
    /* Reset */
    "* { font-size: 12px; }"

    /* Chrome */
    ".chrome {"
    "  background: #0e0e10;"
    "  padding: 0px;"
    "  gap: 0px;"
    "}"

    /* Status bar */
    ".status-bar {"
    "  background: #111118;"
    "  width: 100%;"
    "  height: 20px;"
    "  padding: 0px 7px;"
    "  gap: 8px;"
    "  align-items: center;"
    "  border-top: 2px #070709;"
    "  border-bottom: 2px #3c3c48;"
    "  overflow: hidden;"
    "}"
    ".status-left { flex-grow: 1; gap: 7px; align-items: center; }"
    ".status-right { gap: 9px; align-items: center; }"
    ".status-badge {"
    "  background: #163254;"
    "  width: 15px;"
    "  height: 14px;"
    "  align-items: center;"
    "  justify-content: center;"
    "}"
    ".status-badge-text { color: #b0c8e0; font-size: 10px; }"
    ".status-text { color: #787898; font-size: 11px; }"
    ".status-value { color: #c8c8d0; font-size: 11px; }"
    ".status-ok   { color: #98c379; font-size: 11px; }"
    ".status-busy { color: #d6b35c; font-size: 11px; }"

    /* Tab bar */
    ".tabs-wrap {"
    "  width: 100%;"
    "  height: 24px;"
    "  background: #1e1e26;"
    "  padding: 2px 5px 0px 5px;"
    "  gap: 2px;"
    "  align-items: flex-end;"
    "  border-top: 2px #4c4c58;"
    "  border-bottom: 2px #070709;"
    "  overflow: hidden;"
    "}"
    ".tab-btn {"
    "  background: #282832;"
    "  height: 21px;"
    "  padding: 0px 10px;"
    "  align-items: center;"
    "  justify-content: center;"
    "  border-top: 1px #505060;"
    "  border-left: 1px #505060;"
    "  border-right: 1px #070709;"
    "}"
    ".tab-btn-active {"
    "  background: #0d0d11;"
    "  border-top: 1px #070709;"
    "  border-left: 1px #070709;"
    "  border-right: 1px #505060;"
    "}"
    ".tab-label { color: #686878; font-size: 11px; text-wrap: nowrap; }"
    ".tab-label-active { color: #c8c8d0; font-size: 11px; text-wrap: nowrap; }"

    /* Context panel */
    ".ctx-shell { flex-grow: 1; gap: 8px; padding: 10px 12px; }"
    ".ctx-bar { width: 100%; gap: 8px; align-items: center; }"
    ".ctx-input-wrap { flex-grow: 1; height: 26px; }"
    ".ctx-input {"
    "  width: 100%; height: 26px; padding: 0px 8px;"
    "  background: #161a1f; border: 1px #222933; color: #c0caf5; font-size: 12px;"
    "}"
    ".ctx-run-btn {"
    "  height: 26px; padding: 0px 14px; align-items: center; justify-content: center;"
    "  background: #283b57; border: 1px #3a5375;"
    "}"
    ".ctx-run-label { color: #c0caf5; font-size: 11px; text-wrap: nowrap; }"
    ".ctx-hint { color: #565f89; font-size: 11px; }"
    ".ctx-result { gap: 1px; flex-grow: 1; }"
    ".ctx-line { color: #a9b1d6; font-size: 12px; }"
    ".ctx-line-h { color: #7aa2f7; font-size: 12px; }"
    ".ctx-line-cite { color: #9ece6a; font-size: 11px; }"
    ".ctx-line-code { color: #7e8aa8; font-size: 11px; }"

    /* Content */
    ".content {"
    "  background: #0d0d11;"
    "  padding: 0px;"
    "  gap: 0px;"
    "  flex-grow: 1;"
    "  overflow: hidden;"
    "}"
    ".content-pad {"
    "  background: #0d0d11;"
    "  padding: 8px;"
    "  gap: 6px;"
    "  flex-grow: 1;"
    "  overflow: scroll;"
    "}"
    ".graph-shell {"
    "  background: #151518;"
    "  border-width: 1px;"
    "  border-color: #252530;"
    "  flex-grow: 1;"
    "  padding: 0px;"
    "  gap: 0px;"
    "}"
    ".graph-toolbar {"
    "  background: #18181e;"
    "  height: 24px;"
    "  padding: 0px 8px;"
    "  gap: 8px;"
    "  align-items: center;"
    "  border-bottom: 1px #070709;"
    "}"
    ".graph-title { color: #b8b8c8; font-size: 10px; }"
    ".graph-grow { flex-grow: 1; }"
    ".graph-frame {"
    "  background: #0d0d11;"
    "  min-height: 420px;"
    "  flex-grow: 1;"
    "  padding: 0px;"
    "  gap: 0px;"
    "}"
    ".graph-legend {"
    "  background: transparent;"
    "  gap: 8px;"
    "  align-items: center;"
    "}"
    ".graph-node-legend {"
    "  background: transparent;"
    "  gap: 6px;"
    "  align-items: center;"
    "}"
    ".graph-legend-label { color: #a9b1d6; font-size: 10px; }"
    ".graph-legend-item { height: 14px; gap: 4px; align-items: center; }"
    ".graph-swatch-box {"
    "  width: 16px;"
    "  height: 12px;"
    "  align-items: center;"
    "  justify-content: center;"
    "}"
    ".graph-swatch {"
    "  width: 14px;"
    "  height: 2px;"
    "}"
    ".graph-node-swatch {"
    "  width: 5px;"
    "  height: 5px;"
    "}"

    /* List/table tabs */
    ".list-shell { background: #0d0d11; gap: 7px; flex-grow: 1; overflow: hidden; }"
    ".list-toolbar {"
    "  background: #12151a;"
    "  height: 34px;"
    "  padding: 4px 6px;"
    "  gap: 6px;"
    "  align-items: center;"
    "  border: 1px #222933;"
    "}"
    ".list-title { color: #c0caf5; font-size: 12px; width: 68px; }"
    ".list-search {"
    "  flex-grow: 1; height: 24px; padding: 0px 7px;"
    "  background: #0d1117; border: 1px #283241; color: #c0caf5; font-size: 11px;"
    "}"
    ".list-meta { color: #7e8aa8; font-size: 11px; text-wrap: nowrap; }"
    ".page-btn {"
    "  width: 28px; height: 24px; padding: 0px;"
    "  align-items: center; justify-content: center;"
    "  background: #1a2230; border: 1px #2b3748;"
    "}"
    ".page-btn-disabled { background: #11151b; border: 1px #1a2029; }"
    ".page-label { color: #c0caf5; font-size: 12px; }"
    ".page-label-disabled { color: #565f89; font-size: 12px; }"
    ".table-shell { flex-grow: 1; overflow: scroll; border: 1px #1c232e; background: #0b0d12; }"
    ".tbl-head-row { background: #151a22; }"
    ".tbl-hdr { color: #565f89; font-size: 11px; }"
    ".tbl-row { background: #0b0d12; color: #a9b1d6; font-size: 12px; }"
    ".tbl-row-alt { background: #0f131a; color: #a9b1d6; font-size: 12px; }"
    ".tbl-sym  { color: #7aa2f7; font-size: 12px; }"
    ".tbl-file { color: #565f89; font-size: 11px; }"
    ".tbl-kind { color: #9ece6a; font-size: 11px; }"
    ".tbl-scope { color: #7e8aa8; font-size: 11px; }"

    /* Section heading */
    ".section-head {"
    "  color: #565f89;"
    "  font-size: 10px;"
    "  padding: 0px 0px 4px 0px;"
    "}"

    /* Progress bar in status bar */
    ".prog-track {"
    "  background: #1e2530;"
    "  height: 8px;"
    "  width: 120px;"
    "  border: 1px #2a3545;"
    "}"

    /* Files tab */
    ".files-dir {"
    "  color: #7aa2f7;"
    "  font-size: 11px;"
    "  padding: 4px 0px 2px 0px;"
    "}"
    ".files-err  { color: #e0af68; font-size: 11px; }"
    ".files-stat { color: #565f89; font-size: 11px; }"
    ".files-sum  { color: #9ece6a; font-size: 11px; padding: 6px 0px 2px 0px; }";

/* ============================================================
   Event callbacks (called from background threads)
   ============================================================ */
static void on_graph_updated(const CtxEvent *ev, void *user_data)
{
    CTX_UNUSED(ev); CTX_UNUSED(user_data);
    s.graph_updated = true;
    ca_instance_wake();
}

static void ca_on_close(const Ca_Event *ev, void *user_data)
{
    CTX_UNUSED(ev); CTX_UNUSED(user_data);
    s.closing = true;
}

static void ca_on_mouse_move(const Ca_Event *ev, void *user_data)
{
    CTX_UNUSED(user_data);
    if (!ev || !s.force_graph) return;
    s.mouse_x = (float)ev->mouse_pos.x;
    s.mouse_y = (float)ev->mouse_pos.y;
    ctx_force_graph_mouse_move(s.force_graph,
                               s.mouse_x,
                               s.mouse_y);
}

static void ca_on_mouse_button(const Ca_Event *ev, void *user_data)
{
    CTX_UNUSED(user_data);
    if (!ev || !s.force_graph) return;
    ctx_force_graph_mouse_button(s.force_graph,
                                 ev->mouse_button.button,
                                 ev->mouse_button.action,
                                 s.mouse_x,
                                 s.mouse_y);
}

static void ca_on_mouse_scroll(const Ca_Event *ev, void *user_data)
{
    CTX_UNUSED(user_data);
    if (!ev || !s.force_graph) return;
    ctx_force_graph_mouse_scroll(s.force_graph,
                                 (float)ev->mouse_scroll.dx,
                                 (float)ev->mouse_scroll.dy,
                                 s.mouse_x,
                                 s.mouse_y);
}

static void set_active_tab(int tab)
{
    if (tab < 0 || tab >= CTX_TAB_COUNT || s.active_tab == tab) return;
    s.active_tab = tab;
    if (tab == 1 || tab == 2 || tab == 4) request_list_refresh(tab);
    s.content_needs_rebuild = true;
    ca_instance_wake();
}

static void on_tab_click(Ca_Button *button, void *user_data)
{
    CTX_UNUSED(button);
    TabActionCtx *ctx = user_data;
    if (!ctx) return;
    set_active_tab(ctx->tab);
}

static void on_menu_tab(void *user_data)
{
    TabActionCtx *ctx = user_data;
    if (!ctx) return;
    set_active_tab(ctx->tab);
}

static void on_menu_quit(void *user_data)
{
    CTX_UNUSED(user_data);
    s.closing = true;
}

/*
 * Returns true when haystack contains needle using ASCII case-insensitive
 * comparison.
 *
 * haystack  Text to search. NULL is treated as no match.
 * needle    Query string. Empty queries match all rows.
 */
static bool contains_ci(const char *haystack, const char *needle)
{
    if (!needle || !needle[0]) return true;
    if (!haystack) return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               (char)tolower((unsigned char)p[i]) ==
               (char)tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen) return true;
    }
    return false;
}

/*
 * Returns the short filename component for a path.
 *
 * path  Absolute or relative filesystem path.
 */
static const char *path_base_name(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : (path ? path : "");
}

/*
 * Clamps a page index after a filtered count changes.
 *
 * page   Requested zero-based page.
 * total  Number of filtered rows.
 */
static uint32_t clamp_page(uint32_t page, uint32_t total)
{
    uint32_t pages = total ? ((total - 1u) / LIST_PAGE_SIZE) + 1u : 1u;
    return page >= pages ? pages - 1u : page;
}

/*
 * Invalidates the active list tab after search or pagination changes.
 *
 * tab  List tab index to rebuild.
 */
static void invalidate_list_tab(int tab)
{
    if (tab < 0 || tab >= CTX_TAB_COUNT) return;
    request_list_refresh(tab);
    if (s.active_tab == tab) {
        s.content_needs_rebuild = true;
        ca_instance_wake();
    }
}

static void build_tab_button(int tab, const char *label)
{
    bool active = s.active_tab == tab;
    ca_btn_begin(&(Ca_BtnDesc){
        .style = active ? "tab-btn tab-btn-active" : "tab-btn",
        .direction = CA_HORIZONTAL,
        .background = 0u,
        .on_click = on_tab_click,
        .click_data = &g_tab_actions[tab],
    });
        ca_text(&(Ca_TextDesc){
            .text = label,
            .style = active ? "tab-label tab-label-active" : "tab-label",
        });
    ca_btn_end();
}

static void status_bar_builder(Ca_Window *window, void *user_data)
{
    CTX_UNUSED(window); CTX_UNUSED(user_data);

    CtxGraph *g = ctx_indexer_get_graph();
    CtxIndexProgress prog;
    ctx_indexer_get_progress(&prog);

    float frac = 0.0f;
    if (prog.running && prog.total > 0)
        frac = (float)prog.done / (float)prog.total;
    else if (!prog.running)
        frac = 1.0f;

    char sym_buf[32];
    char edge_buf[32];
    snprintf(sym_buf,  sizeof(sym_buf),  "%u", g ? ctx_graph_symbol_count(g) : 0u);
    snprintf(edge_buf, sizeof(edge_buf), "%u", g ? ctx_graph_edge_count(g)   : 0u);

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "status-bar" });

        /* Left — status badge + state label */
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "status-left" });
            ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "status-badge" });
                ca_text(&(Ca_TextDesc){ .text = "C", .style = "status-badge-text" });
            ca_div_end();
            ca_text(&(Ca_TextDesc){
                .text  = prog.running ? "Indexing" : "Ready",
                .style = prog.running ? "status-busy" : "status-ok",
            });
        ca_div_end();

        /* Right — symbol/edge counts + error warning + progress bar */
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "status-right" });
            ca_text(&(Ca_TextDesc){ .text = "symbols", .style = "status-text" });
            ca_text(&(Ca_TextDesc){ .text = sym_buf,   .style = "status-value" });
            ca_text(&(Ca_TextDesc){ .text = "edges",   .style = "status-text" });
            ca_text(&(Ca_TextDesc){ .text = edge_buf,  .style = "status-value" });
            /* Show parse-error count if any files had extraction errors */
            {
                CtxGraphStats gs2; ctx_indexer_get_stats(&gs2);
                if (gs2.errors > 0) {
                    char errbuf[32];
                    snprintf(errbuf, sizeof(errbuf), "%u err", gs2.errors);
                    ca_text(&(Ca_TextDesc){ .text = errbuf, .style = "status-busy" });
                }
            }
            s.prog_bar = ca_progress(&(Ca_ProgressDesc){
                .value     = frac,
                .width     = 120.0f,
                .height    = 8.0f,
                .bar_color = prog.running ? 0x5d91bdffU : 0x3a6b3affU,
                .style     = "prog-track",
                .hidden    = false,
            });
        ca_div_end();

    ca_div_end();
}

/* ============================================================
   Title-bar frame update (lightweight — just refresh labels)
   ============================================================ */
static void on_frame(void *ud)
{
    CTX_UNUSED(ud);

    CtxIndexProgress prog;
    ctx_indexer_get_progress(&prog);

    if (prog.running) {
        if (s.win) ca_window_invalidate_status_bar(s.win);
        ca_instance_wake();
    }

    if (s.graph_updated) {
        s.graph_updated = false;
        CtxGraph *g = ctx_indexer_get_graph();
        if (g) {
            if (s.force_graph) ctx_force_graph_sync(s.force_graph, g);
            if (s.active_tab == 1 || s.active_tab == 2 || s.active_tab == 4)
                request_list_refresh(s.active_tab);
            s.content_needs_rebuild = true;
            if (s.win) ca_window_invalidate_status_bar(s.win);
        }
    }

    if (s.content_needs_rebuild && s.content_div) {
        if (s.tabs_div) {
            ca_div_clear(s.tabs_div);
            build_tabs(s.tabs_div, NULL);
            ca_div_end();
        }
        ca_div_clear(s.content_div);
        build_content(s.content_div, NULL);
        ca_div_end();
        s.content_needs_rebuild = false;
    }

    if (s.force_graph && s.active_tab == 0)
        ctx_force_graph_frame(s.force_graph);
}

/* ============================================================
   Content builder — runs on causality thread, safe to read graph
   ============================================================ */

/* --- Graph tab -------------------------------------------- */
static void graph_legend_item(uint32_t color, const char *label)
{
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "graph-legend-item" });
        ca_div_begin(&(Ca_DivDesc){ .style = "graph-swatch-box" });
        ca_div_begin(&(Ca_DivDesc){
            .width = 14.0f,
            .height = 2.0f,
            .background = color,
            .style = "graph-swatch",
        });
        ca_div_end();
        ca_div_end();
        ca_text(&(Ca_TextDesc){ .text = label, .style = "graph-legend-label" });
    ca_div_end();
}

static void graph_node_legend_item(uint32_t color, const char *label)
{
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "graph-legend-item" });
        ca_div_begin(&(Ca_DivDesc){
            .width = 5.0f,
            .height = 5.0f,
            .background = color,
            .style = "graph-node-swatch",
        });
        ca_div_end();
        ca_text(&(Ca_TextDesc){ .text = label, .style = "graph-legend-label" });
    ca_div_end();
}

static void build_graph_tab(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);

    CtxGraph *g = ctx_indexer_get_graph();
    if (!g) {
        ca_text(&(Ca_TextDesc){ .text = "No graph loaded.", .style = "tbl-file" });
        return;
    }

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "graph-shell" });
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "graph-toolbar" });
            ca_text(&(Ca_TextDesc){ .text = "Dependency graph", .style = "graph-title" });
            ca_spacer(&(Ca_SpacerDesc){ .width = 0.0f, .style = "graph-grow" });
            /* Edge kind legend */
            ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "graph-legend" });
                graph_legend_item(0x7aa2f7b8u, "calls");
                graph_legend_item(0xe0af68a8u, "refs");
                graph_legend_item(0xf7768eb8u, "inherits");
            ca_div_end();
            /* Node kind legend — node color encodes symbol kind; region fill encodes module */
            ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "graph-node-legend" });
                graph_node_legend_item(0x7aa2f7ffu, "fn");
                graph_node_legend_item(0x89b4faffu, "meth");
                graph_node_legend_item(0xf7768effu, "cls");
                graph_node_legend_item(0x9ece6affu, "struct");
                graph_node_legend_item(0xe0af68ffu, "enum");
                graph_node_legend_item(0xbb9af7ffu, "type");
                graph_node_legend_item(0xff9e64ffu, "macro");
                graph_node_legend_item(0x73dacaffu, "ns");
                graph_node_legend_item(0x565f89ffu, "other");
                ca_text(&(Ca_TextDesc){ .text = "|", .style = "graph-legend-label" });
                ca_text(&(Ca_TextDesc){ .text = "region=module", .style = "graph-legend-label" });
            ca_div_end();
        ca_div_end();
        if (s.force_graph) {
            ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "graph-frame" });
                ctx_force_graph_build(s.force_graph);
            ca_div_end();
        }
    ca_div_end();
}

/* --- List tabs -------------------------------------------- */
/*
 * Returns the compact display label for a symbol kind.
 *
 * kind  Symbol kind from the graph.
 */
static const char *kind_name(CtxSymbolKind kind)
{
    static const char *names[] = {
        "fn","method","class","struct","enum","typedef","var","macro","include","ns","?"
    };
    int idx = (int)kind;
    return (idx >= 0 && idx < 10) ? names[idx] : names[10];
}

/*
 * Returns the compact display label for an edge kind.
 *
 * kind  Edge kind from the graph.
 */
static const char *edge_name(CtxEdgeKind kind)
{
    static const char *names[] = { "calls","includes","defines","refs","inherits" };
    int idx = (int)kind;
    return (idx >= 0 && idx < 5) ? names[idx] : "calls";
}

/*
 * Returns the compact display label for a parser language id.
 *
 * lang  Language id stored in the file table.
 */
static const char *lang_abbrev(int lang)
{
    switch (lang) {
    case 1: return "C";
    case 2: return "C++";
    case 3: return "Py";
    case 4: return "JS";
    case 5: return "TS";
    default: return "?";
    }
}

/*
 * Formats a byte count into a compact display string.
 *
 * size  File size in bytes.
 * out   Destination buffer.
 * cap   Destination capacity.
 */
static void format_size(int64_t size, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (size >= 1024 * 1024)
        snprintf(out, cap, "%.1fM", (double)size / (1024.0 * 1024.0));
    else if (size >= 1024)
        snprintf(out, cap, "%.1fK", (double)size / 1024.0);
    else
        snprintf(out, cap, "%"PRId64"B", size);
}

/*
 * Builds a symbol row snapshot from a graph symbol.
 *
 * row  Destination row.
 * sym  Source graph symbol.
 */
static void copy_symbol_row(SymbolListRow *row, const CtxSymbol *sym)
{
    if (!row || !sym) return;
    snprintf(row->name, sizeof(row->name), "%s", sym->name);
    snprintf(row->kind, sizeof(row->kind), "%s", kind_name(sym->kind));
    snprintf(row->file, sizeof(row->file), "%s", path_base_name(sym->file));
    snprintf(row->scope, sizeof(row->scope), "%s", sym->scope[0] ? sym->scope : "-");
    snprintf(row->line, sizeof(row->line), "%u", sym->line);
}

/*
 * Builds a call row snapshot from a graph edge and resolved symbols.
 *
 * row   Destination row.
 * edge  Source graph edge.
 * from  Resolved caller symbol, or NULL.
 * to    Resolved callee symbol, or NULL.
 */
static void copy_call_row(CallListRow *row, const CtxEdgeEntry *edge,
                          const CtxSymbol *from, const CtxSymbol *to)
{
    if (!row || !edge) return;
    snprintf(row->caller, sizeof(row->caller), "%s", from ? from->name : "?");
    snprintf(row->caller_file, sizeof(row->caller_file), "%s", from ? path_base_name(from->file) : "-");
    snprintf(row->callee, sizeof(row->callee), "%s", to ? to->name : "?");
    snprintf(row->callee_file, sizeof(row->callee_file), "%s", to ? path_base_name(to->file) : "-");
    snprintf(row->kind, sizeof(row->kind), "%s", edge_name(edge->kind));
}

/*
 * Builds a file row snapshot from a store record.
 *
 * row   Destination row.
 * file  Source file record.
 */
static void copy_file_row(FileListRow *row, const CtxFileRecord *file)
{
    if (!row || !file) return;
    const char *slash = strrchr(file->path, '/');
    snprintf(row->file, sizeof(row->file), "%s", path_base_name(file->path));
    snprintf(row->lang, sizeof(row->lang), "%s", lang_abbrev(file->lang));
    snprintf(row->symbols, sizeof(row->symbols), "%d", file->sym_count);
    format_size(file->size, row->size, sizeof(row->size));
    if (file->error_count > 0) snprintf(row->errors, sizeof(row->errors), "%d", file->error_count);
    else snprintf(row->errors, sizeof(row->errors), "-");
    if (slash) {
        size_t dlen = (size_t)(slash - file->path);
        if (dlen >= sizeof(row->dir)) dlen = sizeof(row->dir) - 1;
        memcpy(row->dir, file->path, dlen);
        row->dir[dlen] = '\0';
    } else {
        snprintf(row->dir, sizeof(row->dir), ".");
    }
}

/*
 * Updates the saved search text for a list tab.
 *
 * input  Text input that changed.
 * ud     Tab index stored as TabActionCtx.
 */
static void on_list_search_change(Ca_TextInput *input, void *ud)
{
    TabActionCtx *ctx = ud;
    if (!ctx || ctx->tab < 0 || ctx->tab >= CTX_TAB_COUNT) return;
    const char *text = ca_input_text(input);
    snprintf(s.list_query[ctx->tab], sizeof(s.list_query[ctx->tab]), "%s", text ? text : "");
    s.list_page[ctx->tab] = 0;
    invalidate_list_tab(ctx->tab);
}

/*
 * Applies a one-page forward/backward movement to a list tab.
 *
 * button  Button that was clicked.
 * ud      PageActionCtx with target tab and signed delta.
 */
static void on_page_click(Ca_Button *button, void *ud)
{
    CTX_UNUSED(button);
    PageActionCtx *ctx = ud;
    if (!ctx || ctx->tab < 0 || ctx->tab >= CTX_TAB_COUNT) return;
    if (ctx->delta < 0) {
        if (s.list_page[ctx->tab] > 0) s.list_page[ctx->tab]--;
    } else {
        s.list_page[ctx->tab]++;
    }
    invalidate_list_tab(ctx->tab);
}

/*
 * Draws the common search and pagination controls for list tabs.
 *
 * tab          Tab index owning the controls.
 * title        Visible compact title.
 * placeholder  Search input placeholder.
 * total        Filtered row count.
 */
static void build_list_toolbar(int tab, const char *title, const char *placeholder, uint32_t total)
{
    uint32_t requested_page = s.list_page[tab];
    uint32_t page = clamp_page(requested_page, total);
    s.list_page[tab] = page;
    if (page != requested_page) request_list_refresh(tab);
    uint32_t pages = total ? ((total - 1u) / LIST_PAGE_SIZE) + 1u : 1u;
    uint32_t first = total ? page * LIST_PAGE_SIZE + 1u : 0u;
    uint32_t last = total ? page * LIST_PAGE_SIZE + LIST_PAGE_SIZE : 0u;
    if (last > total) last = total;

    char meta[96];
    snprintf(meta, sizeof(meta), "%u-%u of %u  page %u/%u", first, last, total, page + 1u, pages);

    PageActionCtx *prev = NULL;
    PageActionCtx *next = NULL;
    for (size_t i = 0; i < sizeof(g_page_actions) / sizeof(g_page_actions[0]); i++) {
        if (g_page_actions[i].tab == tab && g_page_actions[i].delta < 0) prev = &g_page_actions[i];
        if (g_page_actions[i].tab == tab && g_page_actions[i].delta > 0) next = &g_page_actions[i];
    }

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "list-toolbar" });
        ca_text(&(Ca_TextDesc){ .text = title, .style = "list-title" });
        ca_input(&(Ca_InputDesc){
            .text = s.list_query[tab][0] ? s.list_query[tab] : NULL,
            .placeholder = placeholder,
            .on_change = on_list_search_change,
            .change_data = &g_tab_actions[tab],
            .style = "list-search",
        });
        ca_text(&(Ca_TextDesc){ .text = meta, .style = "list-meta" });
        bool can_prev = page > 0;
        ca_btn_begin(&(Ca_BtnDesc){
            .style = can_prev ? "page-btn" : "page-btn page-btn-disabled",
            .direction = CA_HORIZONTAL,
            .on_click = can_prev ? on_page_click : NULL,
            .click_data = prev,
            .disabled = !can_prev,
        });
            ca_text(&(Ca_TextDesc){ .text = "<", .style = can_prev ? "page-label" : "page-label-disabled" });
        ca_btn_end();
        bool can_next = page + 1u < pages;
        ca_btn_begin(&(Ca_BtnDesc){
            .style = can_next ? "page-btn" : "page-btn page-btn-disabled",
            .direction = CA_HORIZONTAL,
            .on_click = can_next ? on_page_click : NULL,
            .click_data = next,
            .disabled = !can_next,
        });
            ca_text(&(Ca_TextDesc){ .text = ">", .style = can_next ? "page-label" : "page-label-disabled" });
        ca_btn_end();
    ca_div_end();
}

/*
 * Returns true when a symbol matches the current Symbols tab search.
 *
 * sym    Symbol to test.
 * query  Search string, already stored in tab state.
 */
static bool symbol_matches_query(const CtxSymbol *sym, const char *query)
{
    if (!sym || !sym->is_definition) return false;
    if (!query || !query[0]) return true;
    return contains_ci(sym->name, query) ||
           contains_ci(sym->file, query) ||
           contains_ci(sym->scope, query) ||
           contains_ci(sym->signature, query) ||
           contains_ci(kind_name(sym->kind), query);
}

/*
 * Builds the Symbols tab table with full-result search and pagination.
 *
 * div  Unused active content parent.
 * ud   Unused builder data.
 */
static void build_symbols_tab(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);

    ui_lock();
    ListSnapshot snapshot = s.list_snapshots[1];
    bool needs_refresh = !snapshot.meta.ready && !snapshot.meta.running;
    ui_unlock();
    if (needs_refresh) request_list_refresh(1);

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "list-shell" });
        build_list_toolbar(1, "Symbols", "Search symbols, kinds, files, scopes", snapshot.meta.total);
        if (!snapshot.meta.ready) {
            ca_text(&(Ca_TextDesc){ .text = "Loading symbols...", .style = "tbl-file" });
            ca_div_end();
            return;
        }
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "table-shell" });
            static const float cols[] = { 210, 62, 170, 150, 50 };
            ca_table_begin(&(Ca_TableDesc){ .column_count = 5, .column_widths = cols });
                ca_table_row_begin(&(Ca_DivDesc){ .style = "tbl-head-row" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Name",  .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Kind",  .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "File",  .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Scope", .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Line",  .style = "tbl-hdr" });
                ca_table_row_end();

                for (uint32_t row = 0; row < snapshot.meta.row_count; row++) {
                    const SymbolListRow *item = &snapshot.rows.symbols[row];
                    const char *row_style = (row % 2 == 0) ? "tbl-row" : "tbl-row-alt";
                    ca_table_row_begin(&(Ca_DivDesc){ .style = row_style });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->name, .style = "tbl-sym" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->kind, .style = "tbl-kind" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->file, .style = "tbl-file" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->scope, .style = "tbl-scope" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->line, .style = "tbl-file" });
                    ca_table_row_end();
                }
            ca_table_end();
        ca_div_end();
    ca_div_end();
}

/*
 * Returns true when a call edge matches the current Calls tab search.
 *
 * from   Resolved source symbol, or NULL.
 * to     Resolved target symbol, or NULL.
 * edge   Edge metadata.
 * query  Search string, already stored in tab state.
 */
static bool call_matches_query(const CtxSymbol *from, const CtxSymbol *to,
                               const CtxEdgeEntry *edge, const char *query)
{
    if (!edge || edge->kind != CTX_EDGE_CALLS) return false;
    if (!query || !query[0]) return true;
    return contains_ci(from ? from->name : "?", query) ||
           contains_ci(from ? from->file : "", query) ||
           contains_ci(to ? to->name : "?", query) ||
           contains_ci(to ? to->file : "", query) ||
           contains_ci(edge_name(edge->kind), query);
}

/*
 * Builds the Calls tab table with full-result search and pagination.
 *
 * div  Unused active content parent.
 * ud   Unused builder data.
 */
static void build_calls_tab(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);

    ui_lock();
    ListSnapshot snapshot = s.list_snapshots[2];
    bool needs_refresh = !snapshot.meta.ready && !snapshot.meta.running;
    ui_unlock();
    if (needs_refresh) request_list_refresh(2);

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "list-shell" });
        build_list_toolbar(2, "Calls", "Search caller, callee, or file", snapshot.meta.total);
        if (!snapshot.meta.ready) {
            ca_text(&(Ca_TextDesc){ .text = "Loading calls...", .style = "tbl-file" });
            ca_div_end();
            return;
        }
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "table-shell" });
            static const float cols[] = { 190, 150, 190, 150, 58 };
            ca_table_begin(&(Ca_TableDesc){ .column_count = 5, .column_widths = cols });
                ca_table_row_begin(&(Ca_DivDesc){ .style = "tbl-head-row" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Caller",      .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Caller File", .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Callee",      .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Callee File", .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Kind",        .style = "tbl-hdr" });
                ca_table_row_end();

                for (uint32_t row = 0; row < snapshot.meta.row_count; row++) {
                    const CallListRow *item = &snapshot.rows.calls[row];
                    const char *row_style = (row % 2 == 0) ? "tbl-row" : "tbl-row-alt";
                    ca_table_row_begin(&(Ca_DivDesc){ .style = row_style });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->caller, .style = "tbl-sym" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->caller_file, .style = "tbl-file" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->callee, .style = "tbl-sym" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->callee_file, .style = "tbl-file" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->kind, .style = "tbl-kind" });
                    ca_table_row_end();
                }
            ca_table_end();
        ca_div_end();
    ca_div_end();
}

/*
 * Returns true when a file record matches the current Files tab search.
 *
 * file   Store file record to test.
 * query  Search string, already stored in tab state.
 */
static bool file_matches_query(const CtxFileRecord *file, const char *query)
{
    if (!file) return false;
    if (!query || !query[0]) return true;
    return contains_ci(file->path, query) ||
           contains_ci(path_base_name(file->path), query) ||
           contains_ci(lang_abbrev(file->lang), query);
}

/*
 * Returns true when a list snapshot job has been superseded.
 *
 * job  List job being executed by a worker.
 */
static bool list_job_cancelled(const ListJobData *job)
{
    if (!job) return true;
    bool cancelled;
    ui_lock();
    cancelled = s.closing ||
                s.list_generation[job->tab] != job->generation ||
                strcmp(s.list_query[job->tab], job->query) != 0;
    ui_unlock();
    return cancelled;
}

/*
 * Publishes a completed list snapshot if it is still current.
 *
 * snapshot  Worker-built snapshot.
 */
static void publish_list_snapshot(const ListSnapshot *snapshot)
{
    if (!snapshot) return;
    int tab = snapshot->meta.tab;
    if (!(tab == 1 || tab == 2 || tab == 4)) return;

    bool should_wake = false;
    ui_lock();
    if (!s.closing &&
        s.list_generation[tab] == snapshot->meta.generation &&
        !strcmp(s.list_query[tab], snapshot->meta.query)) {
        s.list_snapshots[tab] = *snapshot;
        s.list_snapshots[tab].meta.running = false;
        s.content_needs_rebuild = true;
        should_wake = true;
    }
    ui_unlock();
    if (should_wake) ca_instance_wake();
}

/*
 * Worker entry for building list tab snapshots off the UI thread.
 *
 * ud  Owned ListJobData allocated by request_list_refresh().
 */
static void list_snapshot_job(void *ud)
{
    ListJobData *job = ud;
    if (!job) return;

    ListSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.meta.tab = job->tab;
    snapshot.meta.generation = job->generation;
    snapshot.meta.page = job->page;
    snapshot.meta.ready = true;
    snprintf(snapshot.meta.query, sizeof(snapshot.meta.query), "%s", job->query);

    uint32_t begin = job->page * LIST_PAGE_SIZE;
    uint32_t end = begin + LIST_PAGE_SIZE;

    if (job->tab == 1) {
        CtxGraph *g = ctx_indexer_get_graph();
        if (g) {
            uint32_t match = 0;
            uint32_t scanned = 0;
            ctx_graph_rlock(g);
            CtxSymbol *sym, *stmp;
            HASH_ITER(hh, g->symbols, sym, stmp) {
                if ((++scanned & 255u) == 0 && list_job_cancelled(job)) break;
                if (!symbol_matches_query(sym, job->query)) continue;
                if (match >= begin && match < end && snapshot.meta.row_count < LIST_PAGE_SIZE)
                    copy_symbol_row(&snapshot.rows.symbols[snapshot.meta.row_count++], sym);
                match++;
            }
            ctx_graph_runlock(g);
            snapshot.meta.total = match;
        }
    } else if (job->tab == 2) {
        CtxGraph *g = ctx_indexer_get_graph();
        if (g) {
            uint32_t match = 0;
            uint32_t scanned = 0;
            ctx_graph_rlock(g);
            CtxEdgeEntry *edge, *etmp;
            HASH_ITER(hh, g->edges, edge, etmp) {
                if ((++scanned & 255u) == 0 && list_job_cancelled(job)) break;
                CtxSymbol *from = NULL, *to = NULL;
                HASH_FIND(hh, g->symbols, &edge->from_id, sizeof(uint64_t), from);
                HASH_FIND(hh, g->symbols, &edge->to_id, sizeof(uint64_t), to);
                if (!call_matches_query(from, to, edge, job->query)) continue;
                if (match >= begin && match < end && snapshot.meta.row_count < LIST_PAGE_SIZE)
                    copy_call_row(&snapshot.rows.calls[snapshot.meta.row_count++], edge, from, to);
                match++;
            }
            ctx_graph_runlock(g);
            snapshot.meta.total = match;
        }
    } else if (job->tab == 4) {
        CtxGraphStats stats;
        ctx_indexer_get_stats(&stats);
        uint32_t file_cap = stats.files ? stats.files + 64u : 512u;
        CtxFileRecord *files = calloc(file_cap, sizeof(*files));
        if (files) {
            uint32_t nfiles = ctx_store_enumerate_files(files, file_cap, ctx_indexer_get_graph());
            uint32_t match = 0;
            for (uint32_t i = 0; i < nfiles; i++) {
                if ((i & 255u) == 0 && list_job_cancelled(job)) break;
                if (!file_matches_query(&files[i], job->query)) continue;
                if (match >= begin && match < end && snapshot.meta.row_count < LIST_PAGE_SIZE)
                    copy_file_row(&snapshot.rows.files[snapshot.meta.row_count++], &files[i]);
                match++;
            }
            snapshot.meta.total = match;
            free(files);
        }
    }

    publish_list_snapshot(&snapshot);
    free(job);
}

/*
 * Requests an asynchronous refresh of a list tab snapshot.
 *
 * tab  List tab index.
 */
static void request_list_refresh(int tab)
{
    if (!(tab == 1 || tab == 2 || tab == 4)) return;

    ListJobData *job = calloc(1, sizeof(*job));
    if (!job) return;

    ui_lock();
    uint64_t generation = ++s.list_generation[tab];
    s.list_snapshots[tab].meta.tab = tab;
    s.list_snapshots[tab].meta.generation = generation;
    s.list_snapshots[tab].meta.page = s.list_page[tab];
    s.list_snapshots[tab].meta.ready = false;
    s.list_snapshots[tab].meta.running = true;
    snprintf(s.list_snapshots[tab].meta.query, sizeof(s.list_snapshots[tab].meta.query),
             "%s", s.list_query[tab]);

    job->tab = tab;
    job->page = s.list_page[tab];
    job->generation = generation;
    snprintf(job->query, sizeof(job->query), "%s", s.list_query[tab]);
    ui_unlock();

    if (!ctx_job_submit(list_snapshot_job, job, CTX_JOB_PRIORITY_HIGH)) {
        ui_lock();
        s.list_snapshots[tab].meta.running = false;
        ui_unlock();
        free(job);
    }
}

/*
 * Builds the Files tab table from store records with full-result search and
 * pagination.
 *
 * div  Unused active content parent.
 * ud   Unused builder data.
 */
static void build_files_tab(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);

    CtxGraphStats stats;
    ctx_indexer_get_stats(&stats);

    ui_lock();
    ListSnapshot snapshot = s.list_snapshots[4];
    bool needs_refresh = !snapshot.meta.ready && !snapshot.meta.running;
    ui_unlock();
    if (needs_refresh) request_list_refresh(4);

    char sum[256];
    snprintf(sum, sizeof(sum),
             "%u files · %u symbols · %u edges · %"PRId64" ms last index",
             stats.files, stats.symbols, stats.edges, stats.duration_ms);

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "list-shell" });
        ca_text(&(Ca_TextDesc){ .text = sum, .style = "files-sum" });
        build_list_toolbar(4, "Files", "Search paths or language", snapshot.meta.total);
        if (!snapshot.meta.ready) {
            ca_text(&(Ca_TextDesc){ .text = "Loading files...", .style = "tbl-file" });
            ca_div_end();
            return;
        }
        if (snapshot.meta.total == 0) {
            ca_text(&(Ca_TextDesc){ .text = "No matching files.", .style = "tbl-file" });
            ca_div_end();
            return;
        }

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "table-shell" });
            static const float cols[] = { 300, 52, 54, 74, 54, 190 };
            ca_table_begin(&(Ca_TableDesc){ .column_count = 6, .column_widths = cols });
                ca_table_row_begin(&(Ca_DivDesc){ .style = "tbl-head-row" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "File",   .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Lang",   .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Syms",   .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Size",   .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Errors", .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Directory", .style = "tbl-hdr" });
                ca_table_row_end();

                for (uint32_t row = 0; row < snapshot.meta.row_count; row++) {
                    const FileListRow *item = &snapshot.rows.files[row];
                    const char *row_style = (row % 2 == 0) ? "tbl-row" : "tbl-row-alt";
                    ca_table_row_begin(&(Ca_DivDesc){ .style = row_style });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->file, .style = "tbl-file" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->lang, .style = "tbl-kind" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->symbols, .style = "tbl-sym" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->size, .style = "files-stat" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->errors, .style = strcmp(item->errors, "-") ? "files-err" : "files-stat" });
                        ca_table_cell(&(Ca_TextDesc){ .text = item->dir, .style = "tbl-scope" });
                    ca_table_row_end();
                }
            ca_table_end();
        ca_div_end();
    ca_div_end();
}

/* --- Context retrieval tab -------------------------------- */

static void run_ctx_query(void);

static void on_ctx_query_change(Ca_TextInput *input, void *ud)
{
    CTX_UNUSED(ud);
    const char *text = ca_input_text(input);
    if (!text) { s.ctx_query[0] = '\0'; return; }
    strncpy(s.ctx_query, text, sizeof(s.ctx_query) - 1);
    s.ctx_query[sizeof(s.ctx_query) - 1] = '\0';
}

/*
 * Worker entry for context retrieval requests.
 *
 * ud  Owned ContextJobData allocated by run_ctx_query().
 */
static void context_query_job(void *ud)
{
    ContextJobData *job = ud;
    if (!job) return;

    CtxRetrieveRequest req = { .kind = CTX_QUERY_TASK, .text = job->query };
    char *result = ctx_retrieve(ctx_indexer_get_graph(), &req);

    bool should_wake = false;
    ui_lock();
    if (!s.closing && s.ctx_generation == job->generation) {
        free(s.ctx_result);
        s.ctx_result = result;
        result = NULL;
        s.ctx_running = false;
        s.content_needs_rebuild = true;
        should_wake = true;
    }
    ui_unlock();

    free(result);
    free(job);
    if (should_wake) ca_instance_wake();
}

static void run_ctx_query(void)
{
    if (s.ctx_query[0] == '\0') return;

    ContextJobData *job = calloc(1, sizeof(*job));
    if (!job) return;

    ui_lock();
    uint64_t generation = ++s.ctx_generation;
    s.ctx_running = true;
    free(s.ctx_result);
    s.ctx_result = NULL;
    job->generation = generation;
    snprintf(job->query, sizeof(job->query), "%s", s.ctx_query);
    ui_unlock();

    if (!ctx_job_submit(context_query_job, job, CTX_JOB_PRIORITY_HIGH)) {
        ui_lock();
        if (s.ctx_generation == generation) s.ctx_running = false;
        ui_unlock();
        free(job);
    }

    s.content_needs_rebuild = true;
    ca_instance_wake();
}

static void on_ctx_run_click(Ca_Button *button, void *ud)
{
    CTX_UNUSED(button); CTX_UNUSED(ud);
    run_ctx_query();
}

/* Line styling for the structured output format:
 * MODULE/FILE headers → blue, edge/sig lines (indented) → muted, rest → normal */
static const char *ctx_line_style(const char *line)
{
    /* Top-level keyword headers */
    if (!strncmp(line, "CODEBASE:", 9) || !strncmp(line, "QUERY:", 6)  ||
        !strncmp(line, "TERMS:", 6)    || !strncmp(line, "SYMBOLS:", 8) ||
        !strncmp(line, "MODULES:", 8)  || !strncmp(line, "MODULE ", 7))
        return "ctx-line-h";
    /* File lines (2 spaces + "FILE ") */
    if (!strncmp(line, "  FILE ", 7)) return "ctx-line-cite";
    /* Module map entries and all other indented content */
    if (line[0] == ' ' || line[0] == '\t') return "ctx-line-code";
    return "ctx-line";
}

static void build_context_tab(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "ctx-shell" });
        ca_text(&(Ca_TextDesc){
            .text = "Playground — preview the full relational context block an agent "
                    "receives from GET /context. Deep graph traversal: call chains, "
                    "reference sites, module structure. No token budget, no truncation. "
                    "No LLM is called.",
            .style = "ctx-hint", .wrap = true });

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "ctx-bar" });
            ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "ctx-input-wrap" });
                s.ctx_query_input = ca_input(&(Ca_InputDesc){
                    .text        = s.ctx_query[0] ? s.ctx_query : NULL,
                    .placeholder = "Describe a task — e.g. how does culling work",
                    .on_change   = on_ctx_query_change,
                    .style       = "ctx-input",
                });
            ca_div_end();
            ca_btn_begin(&(Ca_BtnDesc){
                .style = "ctx-run-btn", .direction = CA_HORIZONTAL,
                .background = 0u, .on_click = on_ctx_run_click,
            });
                ca_text(&(Ca_TextDesc){ .text = "Retrieve", .style = "ctx-run-label" });
            ca_btn_end();
        ca_div_end();

        if (s.ctx_query_input &&
            ca_input_key_pressed(s.ctx_query_input, CTX_KEY_ENTER))
            run_ctx_query();

        ui_lock();
        bool ctx_running = s.ctx_running;
        char *ctx_result = s.ctx_result ? strdup(s.ctx_result) : NULL;
        ui_unlock();

        if (!ctx_result) {
            ca_text(&(Ca_TextDesc){
                .text = ctx_running ? "Retrieving context..." :
                        "Type a task and press Enter (or click Retrieve).",
                .style = "ctx-hint", .wrap = true });
            ca_div_end();
            return;
        }

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "ctx-result" });
            const char *p = ctx_result;
            char line[512];
            int rendered = 0;
            while (*p && rendered < 400) {
                const char *nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                if (len >= sizeof(line)) len = sizeof(line) - 1;
                memcpy(line, p, len);
                line[len] = '\0';
                ca_text(&(Ca_TextDesc){
                    .text = line[0] ? line : " ",
                    .style = ctx_line_style(line), .wrap = true });
                rendered++;
                if (!nl) break;
                p = nl + 1;
            }
        ca_div_end();
        free(ctx_result);
    ca_div_end();
}

/* --- Master content builder ------------------------------- */
/*
 * Builds the sticky tab strip.
 *
 * div  Unused tab parent.
 * ud   Unused builder data.
 */
static void build_tabs(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);
    ca_div_begin(&(Ca_DivDesc){ .style = "tabs-wrap" });
        build_tab_button(0, "Graph");
        build_tab_button(1, "Symbols");
        build_tab_button(2, "Calls");
        build_tab_button(3, "Context");
        build_tab_button(4, "Files");
    ca_div_end();
}

/*
 * Builds the scrollable body for the active tab.
 *
 * div  Unused content parent.
 * ud   Unused builder data.
 */
static void build_content(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);
    int tab = s.active_tab;
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "content-pad" });
        if      (tab == 0) build_graph_tab  (NULL, NULL);
        else if (tab == 1) build_symbols_tab(NULL, NULL);
        else if (tab == 2) build_calls_tab  (NULL, NULL);
        else if (tab == 3) build_context_tab(NULL, NULL);
        else               build_files_tab  (NULL, NULL);
    ca_div_end();
}

/* ============================================================
   Entry point
   ============================================================ */
bool ctx_ui_run(void)
{
#if defined(CTX_PLATFORM_WINDOWS)
    InitializeCriticalSection(&s_ui_lock);
#endif
    s.active_tab = 0;
    s.force_graph = ctx_force_graph_create();
    if (!s.force_graph) {
        CTX_LOG_ERROR("Failed to allocate graph viewport");
#if defined(CTX_PLATFORM_WINDOWS)
        DeleteCriticalSection(&s_ui_lock);
#endif
        return false;
    }

    Ca_InstanceDesc inst_desc = {
        .app_name             = "ctx",
        .prefer_dedicated_gpu = true,
    };
    s.inst = ca_instance_create(&inst_desc);
    if (!s.inst) {
        CTX_LOG_ERROR("Failed to create causality instance");
        ctx_force_graph_destroy(s.force_graph);
        s.force_graph = NULL;
#if defined(CTX_PLATFORM_WINDOWS)
        DeleteCriticalSection(&s_ui_lock);
#endif
        return false;
    }
    ca_instance_set_continuous(s.inst, true);

    ca_event_set_handler(s.inst, CA_EVENT_WINDOW_CLOSE, ca_on_close, NULL);
    ca_event_set_handler(s.inst, CA_EVENT_MOUSE_MOVE, ca_on_mouse_move, NULL);
    ca_event_set_handler(s.inst, CA_EVENT_MOUSE_BUTTON, ca_on_mouse_button, NULL);
    ca_event_set_handler(s.inst, CA_EVENT_MOUSE_SCROLL, ca_on_mouse_scroll, NULL);

    Ca_Stylesheet *sheet = ca_css_parse(g_css);
    if (sheet) ca_instance_set_stylesheet(s.inst, sheet);

    s.win = ca_window_create(s.inst, &(Ca_WindowDesc){
        .title  = "ctx",
        .width  = 680,
        .height = 500,
    });
    if (!s.win) {
        ctx_force_graph_destroy(s.force_graph);
        s.force_graph = NULL;
        ca_instance_destroy(s.inst);
#if defined(CTX_PLATFORM_WINDOWS)
        DeleteCriticalSection(&s_ui_lock);
#endif
        return false;
    }

    /* Subscribe before building — indexing runs on background thread */
    s.graph_updated = true;
    s.content_needs_rebuild = true;
    ctx_event_subscribe(CTX_EVENT_GRAPH_UPDATED, on_graph_updated, NULL);
    ca_window_set_on_frame(s.win, on_frame, NULL);
    ca_window_set_status_bar(s.win, status_bar_builder, NULL, 20.0f);

    static const Ca_MenuItemDesc file_items[] = {
        { .label = "Quit", .action = on_menu_quit },
    };
    static const Ca_MenuItemDesc view_items[] = {
        { .label = "Graph",   .action = on_menu_tab, .action_data = &g_tab_actions[0] },
        { .label = "Symbols", .action = on_menu_tab, .action_data = &g_tab_actions[1] },
        { .label = "Calls",   .action = on_menu_tab, .action_data = &g_tab_actions[2] },
        { .label = "Context", .action = on_menu_tab, .action_data = &g_tab_actions[3] },
        { .label = "Files",   .action = on_menu_tab, .action_data = &g_tab_actions[4] },
    };
    static const Ca_MenuDesc title_menus[] = {
        { .label = "ctx",  .items = file_items, .item_count = 1 },
        { .label = "View", .items = view_items, .item_count = 5 },
    };
    ca_window_set_title_bar_menus(s.win, title_menus, 2);

    /* ---- Retained UI tree ---- */
    ca_ui_begin(s.win, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "chrome",
    });

        s.tabs_div = ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
        });
        ca_div_end();

        /* -- Content pane (reactive) ---------------------------- */
        s.content_div = ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "content",
            .height    = 0,      /* fill remaining height */
        });
        ca_div_end();

    ca_ui_end();

    while (ca_instance_tick(s.inst)) {
        if (s.closing) ca_window_close(s.win);
    }

    ui_lock();
    s.closing = true;
    ui_unlock();

    ctx_force_graph_destroy(s.force_graph);
    s.force_graph = NULL;
    free(s.ctx_result);
    s.ctx_result = NULL;
    ca_instance_destroy(s.inst);
#if defined(CTX_PLATFORM_WINDOWS)
    DeleteCriticalSection(&s_ui_lock);
#endif
    return true;
}

#endif /* CTX_HAS_CAUSALITY */
