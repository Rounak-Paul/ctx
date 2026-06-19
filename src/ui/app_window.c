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
    Ca_TabBar    *tabs;
    Ca_Div       *content_div;   /* stable div with builder — invalidated on tab change / graph update */
    volatile int  active_tab;    /* 0=Graph 1=Symbols 2=Calls 3=Files */

    /* Stats labels in title bar */
    Ca_Label     *lbl_sym;
    Ca_Label     *lbl_edge;
    Ca_Label     *lbl_status;
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

/* ============================================================
   CSS — flat, sharp (zero corner_radius in code, all via struct fields)
   ============================================================ */
static const char *g_css =
    /* Reset */
    "* { font-size: 12px; }"

    /* Chrome */
    ".chrome {"
    "  background: #0e1013;"
    "  padding: 0px;"
    "  gap: 0px;"
    "}"
    ".titlebar {"
    "  background: #090a0c;"
    "  padding: 5px 12px;"
    "  gap: 16px;"
    "}"
    ".app-name { color: #7aa2f7; font-size: 13px; }"
    ".pill {"
    "  background: #161a1f;"
    "  padding: 2px 8px;"
    "  color: #565f89;"
    "  font-size: 11px;"
    "}"
    ".pill-val { color: #c0caf5; font-size: 11px; }"
    ".status-bar {"
    "  background: #090a0c;"
    "  padding: 3px 12px;"
    "  gap: 8px;"
    "}"
    ".status-text { color: #565f89; font-size: 11px; }"
    ".status-ok   { color: #9ece6a; font-size: 11px; }"
    ".status-busy { color: #e0af68; font-size: 11px; }"

    /* Tab bar */
    ".tabs-wrap { background: #090a0c; padding: 0px 12px; }"
    ".divider { background: #22293300; height: 1px; }"

    /* Content */
    ".content {"
    "  background: #090a0c;"
    "  padding: 16px;"
    "  gap: 12px;"
    "  flex-grow: 1;"
    "}"
    ".graph-frame {"
    "  background: #0e1013;"
    "  border-width: 1px;"
    "  border-color: #222933;"
    "  min-height: 520px;"
    "  flex-grow: 1;"
    "  padding: 1px;"
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

static void on_tab_change(Ca_TabBar *tb, void *ud)
{
    CTX_UNUSED(ud);
    s.active_tab = ca_tabs_active(tb);
    s.content_needs_rebuild = true;
    ca_instance_wake();
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
        ca_progress_set(s.prog_bar, frac);
        ca_instance_wake();
    }

    if (s.graph_updated) {
        s.graph_updated = false;

        CtxGraph *g = ctx_indexer_get_graph();
        if (g) {
            if (s.force_graph) ctx_force_graph_sync(s.force_graph, g);
            s.content_needs_rebuild = true;

            char buf[32];
            snprintf(buf, sizeof(buf), "%u", ctx_graph_symbol_count(g));
            ca_set_text(s.lbl_sym, buf);
            snprintf(buf, sizeof(buf), "%u", ctx_graph_edge_count(g));
            ca_set_text(s.lbl_edge, buf);

            CtxIndexProgress p2;
            ctx_indexer_get_progress(&p2);
            if (p2.running) {
                ca_set_text(s.lbl_status, "Indexing\xe2\x80\xa6");
            } else {
                ca_progress_set(s.prog_bar, 1.0f);
                ca_set_text(s.lbl_status, "Ready");
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
static void build_graph_tab(Ca_Div *div, void *ud)
{
    CTX_UNUSED(div); CTX_UNUSED(ud);

    CtxGraph *g = ctx_indexer_get_graph();
    if (!g) {
        ca_text(&(Ca_TextDesc){ .text = "No graph loaded.", .style = "tbl-file" });
        return;
    }

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "graph-frame" });
        if (s.force_graph)
            ctx_force_graph_build(s.force_graph);
    ca_div_end();

    /* Stats row below the graph */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .gap = 16,
                                .padding   = {8, 0, 0, 0} });
        char buf[64];
        snprintf(buf, sizeof(buf), "Indexed graph: %u symbols / %u edges",
                 ctx_graph_symbol_count(g),
                 ctx_graph_edge_count(g));
        ca_text(&(Ca_TextDesc){ .text = buf, .style = "tbl-file" });
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
        snprintf(buf, sizeof(buf), "%u call edges resolved", stats.edges);
        ca_text(&(Ca_TextDesc){ .text = buf, .style = "tbl-row" });
        snprintf(buf, sizeof(buf), "Indexed in %"PRId64" ms", stats.duration_ms);
        ca_text(&(Ca_TextDesc){ .text = buf, .style = "tbl-file" });
    ca_div_end();
}

/* --- Master content builder ------------------------------- */
static void build_content(Ca_Div *div, void *ud)
{
    CTX_UNUSED(ud);
    int tab = s.active_tab;
    if      (tab == 0) build_graph_tab  (div, NULL);
    else if (tab == 1) build_symbols_tab(div, NULL);
    else if (tab == 2) build_calls_tab  (div, NULL);
    else               build_files_tab  (div, NULL);
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
        .width  = 1280,
        .height = 800,
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

    /* ---- Retained UI tree ---- */
    ca_ui_begin(s.win, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "chrome",
    });

        /* -- Title bar ------------------------------------------ */
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL,
                                    .style     = "titlebar" });
            ca_text(&(Ca_TextDesc){ .text = "ctx", .style = "app-name" });
            ca_text(&(Ca_TextDesc){ .text = "|",   .style = "status-text" });
            ca_text(&(Ca_TextDesc){ .text = "sym", .style = "pill" });
            s.lbl_sym  = ca_text(&(Ca_TextDesc){ .text = "—", .style = "pill-val" });
            ca_text(&(Ca_TextDesc){ .text = "edge", .style = "pill" });
            s.lbl_edge = ca_text(&(Ca_TextDesc){ .text = "—", .style = "pill-val" });
            ca_text(&(Ca_TextDesc){ .text = "|",   .style = "status-text" });
            s.lbl_status = ca_text(&(Ca_TextDesc){ .text = "starting\xe2\x80\xa6",
                                                   .style = "status-busy" });
        ca_div_end();

        /* Progress bar — 2px strip under title bar */
        s.prog_bar = ca_progress(&(Ca_ProgressDesc){
            .value     = 0.0f,
            .width     = 0.0f,
            .height    = 2.0f,
            .bar_color = 0x7aa2f7ff,
            .style     = "prog-track",
        });

        /* -- Tab bar -------------------------------------------- */
        static const char *tab_labels[] = { "Graph", "Symbols", "Calls", "Files" };
        ca_div_begin(&(Ca_DivDesc){ .style = "tabs-wrap" });
            s.tabs = ca_tabs(&(Ca_TabBarDesc){
                .labels        = tab_labels,
                .count         = 4,
                .active        = 0,
                .on_change     = on_tab_change,
                .active_bg     = 0x0e1013ff,
                .inactive_bg   = 0x00000000,
                .active_text   = 0x7aa2f7ff,
                .inactive_text = 0x565f89ff,
                .tab_padding_x = 14,
            });
        ca_div_end();

        /* Hairline separator */
        ca_hr(&(Ca_HrDesc){ .thickness = 1, .color = 0x22293380 });

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
