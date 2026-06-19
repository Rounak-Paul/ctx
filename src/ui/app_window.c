#ifdef CTX_HAS_CAUSALITY

#include "app_window.h"
#include "../log/log.h"
#include "../event/event.h"
#include "../indexer/indexer.h"
#include "../graph/graph.h"
#include "force_graph.h"

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

/* ============================================================
   State
   ============================================================ */
typedef struct {
    Ca_Instance  *inst;
    Ca_Window    *win;

    /* Navigation */
    Ca_Div       *content_div;   /* stable div with builder — invalidated on tab change / graph update */
    volatile int  active_tab;    /* 0=Graph 1=Symbols 2=Calls 3=Files */

    Ca_Progress  *prog_bar;

    /* Interactive graph viewport */
    CtxForceGraph *force_graph;
    float          mouse_x;
    float          mouse_y;
    bool           content_needs_rebuild;

    volatile bool graph_updated;
    volatile bool closing;
} AppState;

static AppState s;

static void build_content(Ca_Div *div, void *ud);

typedef struct {
    int tab;
} TabActionCtx;

static TabActionCtx g_tab_actions[4] = {
    { .tab = 0 },
    { .tab = 1 },
    { .tab = 2 },
    { .tab = 3 },
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
    "  padding: 6px;"
    "  gap: 6px;"
    "  flex-grow: 1;"
    "  overflow: hidden;"
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

    /* Table */
    ".tbl-hdr { color: #565f89; font-size: 11px; }"
    ".tbl-row { color: #a9b1d6; font-size: 12px; }"
    ".tbl-row-alt { background: #0e1013; color: #a9b1d6; font-size: 12px; }"
    ".tbl-sym  { color: #7aa2f7; font-size: 12px; }"
    ".tbl-file { color: #565f89; font-size: 11px; }"
    ".tbl-kind { color: #9ece6a; font-size: 11px; }"

    /* Section heading */
    ".section-head {"
    "  color: #565f89;"
    "  font-size: 10px;"
    "  padding: 0px 0px 4px 0px;"
    "}"

    /* Progress track */
    ".prog-track {"
    "  background: #161a1f;"
    "  height: 2px;"
    "}";

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
    if (tab < 0 || tab > 3 || s.active_tab == tab) return;
    s.active_tab = tab;
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

    char sym_buf[32];
    char edge_buf[32];
    snprintf(sym_buf, sizeof(sym_buf), "%u", g ? ctx_graph_symbol_count(g) : 0u);
    snprintf(edge_buf, sizeof(edge_buf), "%u", g ? ctx_graph_edge_count(g) : 0u);

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "status-bar" });
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "status-left" });
            ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "status-badge" });
                ca_text(&(Ca_TextDesc){ .text = "C", .style = "status-badge-text" });
            ca_div_end();
            ca_text(&(Ca_TextDesc){
                .text = prog.running ? "Indexing" : "Ready",
                .style = prog.running ? "status-busy" : "status-ok",
            });
        ca_div_end();

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "status-right" });
            ca_text(&(Ca_TextDesc){ .text = "symbols", .style = "status-text" });
            ca_text(&(Ca_TextDesc){ .text = sym_buf, .style = "status-value" });
            ca_text(&(Ca_TextDesc){ .text = "edges", .style = "status-text" });
            ca_text(&(Ca_TextDesc){ .text = edge_buf, .style = "status-value" });
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

    if (prog.running && prog.total > 0) {
        float frac = (float)prog.done / (float)prog.total;
        if (s.prog_bar) ca_progress_set(s.prog_bar, frac);
        if (s.win) ca_window_invalidate_status_bar(s.win);
        ca_instance_wake();
    }

    if (s.graph_updated) {
        s.graph_updated = false;

        CtxGraph *g = ctx_indexer_get_graph();
        if (g) {
            if (s.force_graph) ctx_force_graph_sync(s.force_graph, g);
            s.content_needs_rebuild = true;

            CtxIndexProgress p2;
            ctx_indexer_get_progress(&p2);
            if (p2.running) {
                if (s.win) ca_window_invalidate_status_bar(s.win);
            } else {
                if (s.prog_bar) ca_progress_set(s.prog_bar, 1.0f);
                if (s.win) ca_window_invalidate_status_bar(s.win);
            }
        }
    }

    if (s.content_needs_rebuild && s.content_div) {
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
            ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "graph-legend" });
                graph_legend_item(0x7aa2f7b8u, "calls");
                graph_legend_item(0xe0af68a8u, "refs");
                graph_legend_item(0xf7768eb8u, "inherits");
            ca_div_end();
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
            ca_div_end();
        ca_div_end();
        if (s.force_graph) {
            ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "graph-frame" });
                ctx_force_graph_build(s.force_graph);
            ca_div_end();
        }
    ca_div_end();
}

/* --- Symbols tab ------------------------------------------ */
#define MAX_LIST_ROWS 200

static void build_symbols_tab(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);

    CtxGraph *g = ctx_indexer_get_graph();
    if (!g) {
        ca_text(&(Ca_TextDesc){ .text = "No data.", .style = "tbl-file" });
        return;
    }

    /* header */
    static const float cols[] = { 240, 80, 180, 60 };
    ca_table_begin(&(Ca_TableDesc){ .column_count = 4, .column_widths = cols });
        ca_table_row_begin(NULL);
            ca_table_cell(&(Ca_TextDesc){ .text = "Name",      .style = "tbl-hdr" });
            ca_table_cell(&(Ca_TextDesc){ .text = "Kind",      .style = "tbl-hdr" });
            ca_table_cell(&(Ca_TextDesc){ .text = "File",      .style = "tbl-hdr" });
            ca_table_cell(&(Ca_TextDesc){ .text = "Line",      .style = "tbl-hdr" });
        ca_table_row_end();

    static const char *kind_names[] = {
        "fn","method","class","struct","enum","typedef","var","macro","include","ns","?"
    };

    ctx_graph_rlock(g);
    int row = 0;
    CtxSymbol *sym, *stmp;
    HASH_ITER(hh, g->symbols, sym, stmp) {
        if (row >= MAX_LIST_ROWS) break;
        if (!sym->is_definition) continue;
        const char *row_style = (row % 2 == 0) ? "tbl-row" : "tbl-row-alt";
        ca_table_row_begin(&(Ca_DivDesc){ .style = row_style });
            ca_table_cell(&(Ca_TextDesc){ .text = sym->name,      .style = "tbl-sym"  });
            int ki = (int)sym->kind; if (ki < 0 || ki > 10) ki = 10;
            ca_table_cell(&(Ca_TextDesc){ .text = kind_names[ki], .style = "tbl-kind" });
            /* show only the filename part */
            const char *fname = strrchr(sym->file, '/');
            ca_table_cell(&(Ca_TextDesc){ .text = fname ? fname+1 : sym->file, .style = "tbl-file" });
            char lbuf[16]; snprintf(lbuf, sizeof(lbuf), "%u", sym->line);
            ca_table_cell(&(Ca_TextDesc){ .text = lbuf, .style = "tbl-file" });
        ca_table_row_end();
        row++;
    }
    ctx_graph_runlock(g);
    ca_table_end();

    if (row >= MAX_LIST_ROWS) {
        char note[64];
        snprintf(note, sizeof(note), "(showing first %d of %u definitions)",
                 MAX_LIST_ROWS, ctx_graph_symbol_count(g));
        ca_text(&(Ca_TextDesc){ .text = note, .style = "tbl-file" });
    }
}

/* --- Calls tab -------------------------------------------- */
static void build_calls_tab(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);

    CtxGraph *g = ctx_indexer_get_graph();
    if (!g) { ca_text(&(Ca_TextDesc){ .text = "No data.", .style = "tbl-file" }); return; }

    static const float cols[] = { 220, 220, 80 };
    ca_table_begin(&(Ca_TableDesc){ .column_count = 3, .column_widths = cols });
        ca_table_row_begin(NULL);
            ca_table_cell(&(Ca_TextDesc){ .text = "Caller",  .style = "tbl-hdr" });
            ca_table_cell(&(Ca_TextDesc){ .text = "Callee",  .style = "tbl-hdr" });
            ca_table_cell(&(Ca_TextDesc){ .text = "Kind",    .style = "tbl-hdr" });
        ca_table_row_end();

    static const char *edge_kind[] = { "calls","includes","defines","refs","inherits" };

    ctx_graph_rlock(g);
    int row = 0;
    CtxEdgeEntry *e, *etmp;
    HASH_ITER(hh, g->edges, e, etmp) {
        if (row >= MAX_LIST_ROWS) break;
        if (e->kind != CTX_EDGE_CALLS) continue;

        /* resolve names */
        CtxSymbol *from = NULL, *to = NULL;
        HASH_FIND(hh, g->symbols, &e->from_id, sizeof(uint64_t), from);
        HASH_FIND(hh, g->symbols, &e->to_id,   sizeof(uint64_t), to);

        const char *row_style = (row % 2 == 0) ? "tbl-row" : "tbl-row-alt";
        ca_table_row_begin(&(Ca_DivDesc){ .style = row_style });
            ca_table_cell(&(Ca_TextDesc){ .text = from ? from->name : "?", .style = "tbl-sym"  });
            ca_table_cell(&(Ca_TextDesc){ .text = to   ? to->name   : "?", .style = "tbl-sym"  });
            int ek = (int)e->kind; if (ek < 0 || ek > 4) ek = 0;
            ca_table_cell(&(Ca_TextDesc){ .text = edge_kind[ek],           .style = "tbl-kind" });
        ca_table_row_end();
        row++;
    }
    ctx_graph_runlock(g);
    ca_table_end();

    if (row >= MAX_LIST_ROWS) {
        char note[64];
        snprintf(note, sizeof(note), "(showing first %d of %u edges)",
                 MAX_LIST_ROWS, ctx_graph_edge_count(g));
        ca_text(&(Ca_TextDesc){ .text = note, .style = "tbl-file" });
    }
}

/* --- Files tab -------------------------------------------- */
static void build_files_tab(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);

    CtxGraphStats stats;
    ctx_indexer_get_stats(&stats);

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .gap = 6 });
        char buf[128];
        snprintf(buf, sizeof(buf), "%u files indexed", stats.files);
        ca_text(&(Ca_TextDesc){ .text = buf, .style = "tbl-row" });
        snprintf(buf, sizeof(buf), "%u symbols extracted", stats.symbols);
        ca_text(&(Ca_TextDesc){ .text = buf, .style = "tbl-row" });
        snprintf(buf, sizeof(buf), "%u semantic edges resolved", stats.edges);
        ca_text(&(Ca_TextDesc){ .text = buf, .style = "tbl-row" });
        snprintf(buf, sizeof(buf), "Indexed in %"PRId64" ms", stats.duration_ms);
        ca_text(&(Ca_TextDesc){ .text = buf, .style = "tbl-file" });
    ca_div_end();
}

/* --- Master content builder ------------------------------- */
static void build_content(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);
    int tab = s.active_tab;
    ca_div_begin(&(Ca_DivDesc){ .style = "tabs-wrap" });
        build_tab_button(0, "Graph");
        build_tab_button(1, "Symbols");
        build_tab_button(2, "Calls");
        build_tab_button(3, "Files");
    ca_div_end();

    CtxIndexProgress prog;
    ctx_indexer_get_progress(&prog);
    float progress_value = 1.0f;
    if (prog.running && prog.total > 0)
        progress_value = (float)prog.done / (float)prog.total;
    s.prog_bar = ca_progress(&(Ca_ProgressDesc){
        .value     = progress_value,
        .width     = 0.0f,
        .height    = 2.0f,
        .bar_color = 0x5d91bdff,
        .style     = "prog-track",
    });

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "content-pad" });
        if      (tab == 0) build_graph_tab  (NULL, NULL);
        else if (tab == 1) build_symbols_tab(NULL, NULL);
        else if (tab == 2) build_calls_tab  (NULL, NULL);
        else               build_files_tab  (NULL, NULL);
    ca_div_end();
}

/* ============================================================
   Entry point
   ============================================================ */
bool ctx_ui_run(void)
{
    s.active_tab = 0;
    s.force_graph = ctx_force_graph_create();
    if (!s.force_graph) {
        CTX_LOG_ERROR("Failed to allocate graph viewport");
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
        .width  = 1080,
        .height = 680,
    });
    if (!s.win) {
        ctx_force_graph_destroy(s.force_graph);
        s.force_graph = NULL;
        ca_instance_destroy(s.inst);
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
        { .label = "Files",   .action = on_menu_tab, .action_data = &g_tab_actions[3] },
    };
    static const Ca_MenuDesc title_menus[] = {
        { .label = "ctx",  .items = file_items, .item_count = 1 },
        { .label = "View", .items = view_items, .item_count = 4 },
    };
    ca_window_set_title_bar_menus(s.win, title_menus, 2);

    /* ---- Retained UI tree ---- */
    ca_ui_begin(s.win, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "chrome",
    });

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

    ctx_force_graph_destroy(s.force_graph);
    s.force_graph = NULL;
    ca_instance_destroy(s.inst);
    return true;
}

#endif /* CTX_HAS_CAUSALITY */
