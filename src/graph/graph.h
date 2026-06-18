#pragma once
#include "../pch.h"

typedef enum {
    CTX_SYM_FUNCTION = 0, CTX_SYM_METHOD, CTX_SYM_CLASS, CTX_SYM_STRUCT,
    CTX_SYM_ENUM, CTX_SYM_TYPEDEF, CTX_SYM_VARIABLE, CTX_SYM_MACRO,
    CTX_SYM_INCLUDE, CTX_SYM_NAMESPACE, CTX_SYM_UNKNOWN
} CtxSymbolKind;

typedef enum {
    CTX_EDGE_CALLS = 0, CTX_EDGE_INCLUDES, CTX_EDGE_DEFINES,
    CTX_EDGE_REFERENCES, CTX_EDGE_INHERITS
} CtxEdgeKind;

typedef struct {
    uint64_t      id;
    char          name[256];
    char          file[4096];
    uint32_t      line;
    uint32_t      col;
    CtxSymbolKind kind;
    char          signature[512];
    bool          is_definition;
    UT_hash_handle hh;
} CtxSymbol;

typedef struct CtxEdgeEntry {
    uint64_t         key;      /* FNV64(from_id||to_id||kind) — uthash key */
    uint64_t         from_id;
    uint64_t         to_id;
    CtxEdgeKind      kind;
    UT_hash_handle   hh;
} CtxEdgeEntry;

/* Unresolved call: caller site + bare callee name, resolved post-index.
 * Uses heap strings to keep the array compact (~28 bytes/entry vs 4612). */
typedef struct {
    char    *caller_file;   /* owned, must free */
    char    *caller_name;   /* owned, may be NULL */
    char    *callee_name;   /* owned */
    uint32_t caller_line;
} CtxPendingCall;

typedef struct {
    CtxSymbol    *symbols;    /* uthash by id */
    CtxEdgeEntry *edges;      /* uthash by composite FNV key stored in from_id field */

    /* Pending call list — populated during extraction, resolved post-index */
    CtxPendingCall *pending_calls;
    uint32_t        pending_count;
    uint32_t        pending_cap;

#if defined(CTX_PLATFORM_WINDOWS)
    SRWLOCK          lock;
    CRITICAL_SECTION pending_lock;
#else
    pthread_rwlock_t lock;
    pthread_mutex_t  pending_lock;
#endif
} CtxGraph;

uint64_t  ctx_fnv64(const char *data, size_t len);
uint64_t  ctx_symbol_id(const char *file, const char *name, uint32_t line);

CtxGraph *ctx_graph_create(void);
void      ctx_graph_destroy(CtxGraph *g);
void      ctx_graph_add_symbol(CtxGraph *g, const CtxSymbol *sym);
void      ctx_graph_add_edge(CtxGraph *g, uint64_t from_id, uint64_t to_id, CtxEdgeKind kind);
void      ctx_graph_remove_file(CtxGraph *g, const char *path);
uint32_t  ctx_graph_symbol_count(CtxGraph *g);
uint32_t  ctx_graph_edge_count(CtxGraph *g);
CtxSymbol *ctx_graph_find_by_name(CtxGraph *g, const char *name); /* first match */

void ctx_graph_rlock(CtxGraph *g);
void ctx_graph_runlock(CtxGraph *g);
void ctx_graph_wlock(CtxGraph *g);
void ctx_graph_wunlock(CtxGraph *g);

/* Accumulate an unresolved call during extraction (thread-safe) */
void ctx_graph_add_pending_call(CtxGraph *g, const char *caller_file,
                                const char *caller_name, uint32_t caller_line,
                                const char *callee_name);

/* Resolve all pending calls against the symbol table; called post-index.
 * Returns the number of edges added. */
uint32_t ctx_graph_resolve_calls(CtxGraph *g);
