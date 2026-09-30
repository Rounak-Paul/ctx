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

/*
 * Indexed symbol. Strings are never NULL and live in the same allocation as
 * the symbol; file is interned in the owning CtxGraphFile. All of them stay
 * valid while the symbol is in the graph (hold at least the read lock).
 */
typedef struct {
    uint64_t       id;
    const char    *name;
    const char    *file;
    const char    *signature;
    const char    *scope;        /* enclosing class/struct/namespace, "" at file scope */
    uint32_t       line;
    uint32_t       col;
    uint32_t       end_line;     /* last source line of the symbol body (>= line) */
    CtxSymbolKind  kind;
    uint8_t        lang;         /* CtxLanguage of the owning file */
    bool           is_definition;
    UT_hash_handle hh;           /* CtxGraph.symbols, keyed by id */
} CtxSymbol;

/* Fixed-buffer symbol produced by extraction and store loading. */
typedef struct {
    uint64_t      id;
    char          name[256];
    char          signature[512];
    char          scope[256];
    uint32_t      line;
    uint32_t      col;
    uint32_t      end_line;
    CtxSymbolKind kind;
    uint8_t       lang;
    bool          is_definition;
} CtxSymbolDraft;

/* Derived edge. refs counts the reference sites currently resolving to it. */
typedef struct CtxEdgeEntry {
    uint64_t       key;          /* FNV64(from_id||to_id||kind) */
    uint64_t       from_id;
    uint64_t       to_id;
    CtxEdgeKind    kind;
    uint32_t       refs;
    UT_hash_handle hh;
} CtxEdgeEntry;

/*
 * Unresolved-by-name reference recorded at extraction time. Sites persist so
 * edges can be re-resolved whenever the set of candidate targets changes.
 *
 * from_name  Enclosing symbol name, NULL at file scope (owned).
 * to_name    Referenced name (owned).
 * from_line  Source line of the reference.
 * res_from   Resolved source symbol id, 0 when unresolved.
 * res_to     Resolved target symbol id, 0 when unresolved.
 */
typedef struct {
    char        *from_name;
    char        *to_name;
    uint32_t     from_line;
    CtxEdgeKind  kind;
    uint64_t     res_from;
    uint64_t     res_to;
} CtxRefSite;

/* Output buffer of one file extraction: symbol drafts plus reference sites. */
typedef struct {
    CtxSymbolDraft *symbols;
    uint32_t    symbol_count;
    uint32_t    symbol_cap;
    CtxRefSite *sites;
    uint32_t    site_count;
    uint32_t    site_cap;
} CtxFileExtract;

/* Per-file graph entry. symbols are ordered by (line, col). version changes
 * (monotonically, graph-wide) every time the file's content is replaced. */
typedef struct CtxGraphFile {
    char           *path;
    uint64_t        version;
    CtxSymbol     **symbols;
    uint32_t        symbol_count;
    CtxRefSite     *sites;
    uint32_t        site_count;
    UT_hash_handle  hh;
} CtxGraphFile;

/* All symbols sharing one name, in insertion order. */
typedef struct CtxNameEntry {
    char           *name;
    CtxSymbol     **symbols;
    uint32_t        count;
    uint32_t        cap;
    UT_hash_handle  hh;
} CtxNameEntry;

typedef struct {
    CtxSymbol    *symbols;   /* by id */
    CtxEdgeEntry *edges;     /* by composite key */
    CtxGraphFile *files;     /* by path */
    CtxNameEntry *names;     /* by name */
    uint64_t      version_seq;
#if defined(CTX_PLATFORM_WINDOWS)
    SRWLOCK          lock;
#else
    pthread_rwlock_t lock;
#endif
} CtxGraph;

uint64_t  ctx_fnv64(const char *data, size_t len);
uint64_t  ctx_symbol_id(const char *file, const char *name, uint32_t line);

/* Appends a symbol draft to an extraction buffer. Returns false on allocation failure. */
bool ctx_file_extract_add_symbol(CtxFileExtract *ex, const CtxSymbolDraft *sym);

/*
 * Appends a reference site to an extraction buffer.
 *
 * from_name  Enclosing symbol name; NULL or "" for file scope.
 * from_line  Source line of the reference.
 * to_name    Referenced name; ignored when NULL or empty.
 * kind       Edge kind produced once resolved.
 */
bool ctx_file_extract_add_site(CtxFileExtract *ex, const char *from_name,
                               uint32_t from_line, const char *to_name,
                               CtxEdgeKind kind);

/* Releases everything owned by an extraction buffer and zeroes it. */
void ctx_file_extract_free(CtxFileExtract *ex);

CtxGraph *ctx_graph_create(void);
void      ctx_graph_destroy(CtxGraph *g);

/*
 * Atomically replaces everything the graph knows about one file.
 *
 * path     File path (absolute).
 * ex       New content; NULL removes the file. Ownership of ex's arrays moves
 *          into the graph and ex is left empty.
 * resolve  true: resolve the file's sites and re-resolve other files' sites
 *          whose target name was defined in the old or new content.
 *          false: defer to ctx_graph_resolve_all (bulk indexing).
 */
void      ctx_graph_replace_file(CtxGraph *g, const char *path, CtxFileExtract *ex,
                                 bool resolve);

/* Re-resolves every reference site. Returns the number of live edges. */
uint32_t  ctx_graph_resolve_all(CtxGraph *g);

uint32_t  ctx_graph_symbol_count(CtxGraph *g);
uint32_t  ctx_graph_edge_count(CtxGraph *g);
uint32_t  ctx_graph_file_count(CtxGraph *g);

/* Lookups below require the caller to hold at least the read lock. */
CtxSymbol          *ctx_graph_find_by_id_locked(CtxGraph *g, uint64_t id);
const CtxGraphFile *ctx_graph_find_file_locked(CtxGraph *g, const char *path);
const CtxNameEntry *ctx_graph_find_name_locked(CtxGraph *g, const char *name);

void ctx_graph_rlock(CtxGraph *g);
void ctx_graph_runlock(CtxGraph *g);
void ctx_graph_wlock(CtxGraph *g);
void ctx_graph_wunlock(CtxGraph *g);
