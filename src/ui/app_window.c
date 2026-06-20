#ifdef CTX_HAS_CAUSALITY

#include "app_window.h"
#include "../log/log.h"
#include "../event/event.h"
#include "../indexer/indexer.h"
#include "../graph/graph.h"
#include "../retrieve/retrieve.h"
#include "../store/store.h"
#include "force_graph.h"

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

/* ============================================================
   State
   ============================================================ */
typedef struct {
    Ca_Instance  *inst;
    Ca_Window    *win;

    /* Navigation */
    Ca_Div       *content_div;   /* stable div with builder — invalidated on tab change / graph update */
    volatile int  active_tab;    /* 0=Context 1=Graph 2=Symbols 3=Calls 4=Files */

    Ca_Progress  *prog_bar;

    /* Context retrieval playground */
    Ca_TextInput *ctx_query_input;
    char          ctx_query[512];
    char         *ctx_result;    /* heap-allocated structured context block; owned */

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

static TabActionCtx g_tab_actions[5] = {
    { .tab = 0 },
    { .tab = 1 },
    { .tab = 2 },
    { .tab = 3 },
    { .tab = 4 },
};

#define CTX_TAB_COUNT 5

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
    ".ctx-bar { gap: 8px; align-items: center; }"
    ".ctx-input {"
    "  flex-grow: 1; height: 26px; padding: 0px 8px;"
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
            s.content_needs_rebuild = true;
            if (s.win) ca_window_invalidate_status_bar(s.win);
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
#define FILES_TAB_MAX 2048

static const char *lang_abbrev(int lang) {
    switch (lang) {
    case 1: return "C";
    case 2: return "C++";
    case 3: return "Py";
    case 4: return "JS";
    case 5: return "TS";
    default: return "?";
    }
}

static void build_files_tab(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);

    CtxGraphStats stats;
    ctx_indexer_get_stats(&stats);

    CtxGraph *g = ctx_indexer_get_graph();

    /* Enumerate files from the store */
    static CtxFileRecord files[FILES_TAB_MAX];
    uint32_t nfiles = ctx_store_enumerate_files(files, FILES_TAB_MAX, g);

    /* Summary header */
    char sum[256];
    snprintf(sum, sizeof(sum),
             "%u files · %u symbols · %u edges · %"PRId64" ms last index",
             stats.files, stats.symbols, stats.edges, stats.duration_ms);
    ca_text(&(Ca_TextDesc){ .text = sum, .style = "files-sum" });

    if (nfiles == 0) {
        ca_text(&(Ca_TextDesc){ .text = "No files indexed yet.", .style = "tbl-file" });
        return;
    }

    /* Columns: file, lang, syms, size, errors */
    static const float cols[] = { 280, 36, 48, 64, 48 };
    ca_table_begin(&(Ca_TableDesc){ .column_count = 5, .column_widths = cols });
        ca_table_row_begin(NULL);
            ca_table_cell(&(Ca_TextDesc){ .text = "File",    .style = "tbl-hdr" });
            ca_table_cell(&(Ca_TextDesc){ .text = "Lang",    .style = "tbl-hdr" });
            ca_table_cell(&(Ca_TextDesc){ .text = "Syms",    .style = "tbl-hdr" });
            ca_table_cell(&(Ca_TextDesc){ .text = "Size",    .style = "tbl-hdr" });
            ca_table_cell(&(Ca_TextDesc){ .text = "Errors",  .style = "tbl-hdr" });
        ca_table_row_end();

        /* Group by directory — files are already sorted by path */
        char cur_dir[4096] = "";
        int row = 0;
        for (uint32_t i = 0; i < nfiles && row < MAX_LIST_ROWS; i++) {
            /* Compute directory for this file */
            const char *fpath = files[i].path;
            char dir[4096];
            const char *slash = strrchr(fpath, '/');
            if (slash) {
                size_t dlen = (size_t)(slash - fpath);
                if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
                memcpy(dir, fpath, dlen);
                dir[dlen] = '\0';
            } else {
                dir[0] = '.'; dir[1] = '\0';
            }

            if (strcmp(dir, cur_dir) != 0) {
                /* Directory group header row */
                strncpy(cur_dir, dir, sizeof(cur_dir) - 1);
                ca_table_row_begin(&(Ca_DivDesc){ .style = "tbl-row-alt" });
                    char dhead[4096];
                    snprintf(dhead, sizeof(dhead), "%s/", dir);
                    ca_table_cell(&(Ca_TextDesc){ .text = dhead, .style = "files-dir" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "",     .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "",     .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "",     .style = "tbl-hdr" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "",     .style = "tbl-hdr" });
                ca_table_row_end();
            }

            /* File row — show basename only (dir shown in group header) */
            const char *basename = slash ? slash + 1 : fpath;
            char sizebuf[32];
            if (files[i].size >= 1024 * 1024)
                snprintf(sizebuf, sizeof(sizebuf), "%.1fM", (double)files[i].size / (1024.0 * 1024.0));
            else if (files[i].size >= 1024)
                snprintf(sizebuf, sizeof(sizebuf), "%.1fK", (double)files[i].size / 1024.0);
            else
                snprintf(sizebuf, sizeof(sizebuf), "%"PRId64"B", files[i].size);

            char symbuf[16];  snprintf(symbuf, sizeof(symbuf), "%d", files[i].sym_count);
            char errbuf[16];
            if (files[i].error_count > 0)
                snprintf(errbuf, sizeof(errbuf), "%d", files[i].error_count);
            else
                errbuf[0] = '\0';

            const char *row_style = (row % 2 == 0) ? "tbl-row" : "tbl-row-alt";
            ca_table_row_begin(&(Ca_DivDesc){ .style = row_style });
                ca_table_cell(&(Ca_TextDesc){ .text = basename,                    .style = "tbl-file" });
                ca_table_cell(&(Ca_TextDesc){ .text = lang_abbrev(files[i].lang),  .style = "tbl-kind" });
                ca_table_cell(&(Ca_TextDesc){ .text = symbuf,                      .style = "tbl-sym"  });
                ca_table_cell(&(Ca_TextDesc){ .text = sizebuf,                     .style = "files-stat" });
                ca_table_cell(&(Ca_TextDesc){
                    .text  = errbuf[0] ? errbuf : "-",
                    .style = errbuf[0] ? "files-err" : "files-stat",
                });
            ca_table_row_end();
            row++;
        }
    ca_table_end();

    if (nfiles > MAX_LIST_ROWS) {
        char note[64];
        snprintf(note, sizeof(note), "(showing first %d of %u files)", MAX_LIST_ROWS, nfiles);
        ca_text(&(Ca_TextDesc){ .text = note, .style = "tbl-file" });
    }
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

static void run_ctx_query(void)
{
    if (s.ctx_query[0] == '\0') return;
    free(s.ctx_result);
    s.ctx_result = NULL;
    CtxRetrieveRequest req = { .kind = CTX_QUERY_TASK, .text = s.ctx_query };
    s.ctx_result = ctx_retrieve(ctx_indexer_get_graph(), &req);
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
            s.ctx_query_input = ca_input(&(Ca_InputDesc){
                .text        = s.ctx_query[0] ? s.ctx_query : NULL,
                .placeholder = "Describe a task — e.g. how does culling work",
                .on_change   = on_ctx_query_change,
                .style       = "ctx-input",
            });
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

        if (!s.ctx_result) {
            ca_text(&(Ca_TextDesc){
                .text = "Type a task and press Enter (or click Retrieve).",
                .style = "ctx-hint", .wrap = true });
            ca_div_end();
            return;
        }

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "ctx-result" });
            const char *p = s.ctx_result;
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
        build_tab_button(3, "Context");
        build_tab_button(4, "Files");
    ca_div_end();

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
        .width  = 680,
        .height = 500,
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
    free(s.ctx_result);
    s.ctx_result = NULL;
    ca_instance_destroy(s.inst);
    return true;
}

#endif /* CTX_HAS_CAUSALITY */
