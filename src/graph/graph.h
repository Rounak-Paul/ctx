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

/* Scope component recorded for C++ anonymous namespaces; members are private
 * to their translation unit and transparent to qualified lookup. */
#define CTX_ANONYMOUS_SCOPE "(anonymous namespace)"

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
    const char    *scope;        /* "::"-joined enclosing namespaces/classes, "" at file scope */
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
 * Type expressions describe how the static type of an object is obtained;
 * they are evaluated at resolution time, once every file is known.
 *
 *   head  T<path>  declared type          S  this / self
 *         B        super()                V<name>  member of the caller's class
 *         C<path>  result of calling path (constructor or function return)
 *   step  |f<name> field of the current type
 *         |m<name> result of calling method name on the current type
 *
 * Paths are "::"-joined. Example: "Sengine_|mcache" is this->engine_.cache().
 */
#define CTX_TYPE_STEP_SEP '|'

/*
 * Unresolved-by-name reference recorded at extraction time. Sites persist so
 * edges can be re-resolved whenever the set of candidate targets changes.
 * Every string is owned by the site.
 *
 * from_name  Enclosing symbol name, NULL at file scope.
 * to_name    Referenced name without qualifier.
 * to_scope   Explicit "::" qualifier written at the site (ns::Foo in
 *            ns::Foo::bar()), NULL when unqualified.
 * recv       Type expression of the receiver of a member site, NULL if unknown.
 * arg_types  ';'-separated type expressions of call arguments, used for
 *            argument-dependent lookup; NULL when none are known.
 * from_line  Source line of the reference.
 * member     Target is reached through an object (obj.f(), ptr->f()).
 * res_from   Resolved source symbol id, 0 when unresolved.
 * res_to     Resolved target symbol id, 0 when unresolved.
 */
typedef struct {
    char        *from_name;
    char        *to_name;
    char        *to_scope;
    char        *recv;
    char        *arg_types;
    uint32_t     from_line;
    CtxEdgeKind  kind;
    bool         member;
    uint64_t     res_from;
    uint64_t     res_to;
} CtxRefSite;

/* Borrowed-string input for ctx_file_extract_add_site; NULL/"" means absent. */
typedef struct {
    const char  *from_name;
    const char  *to_name;
    const char  *to_scope;
    const char  *recv;
    const char  *arg_types;
    uint32_t     from_line;
    CtxEdgeKind  kind;
    bool         member;
} CtxSiteDraft;

/* Name-lookup declarations that change how C++ names resolve. */
typedef enum {
    CTX_DECL_USING_NAMESPACE = 0, /* using namespace target;                  */
    CTX_DECL_USING,               /* using target;   (name = last component)  */
    CTX_DECL_NAMESPACE_ALIAS,     /* namespace name = target;                 */
    CTX_DECL_TYPE_ALIAS,          /* using name = target; / typedef target name; */
    CTX_DECL_FIELD,               /* data member name; target = type expression */
    CTX_DECL_RETURN               /* function name returns target (type expression) */
} CtxDeclKind;

/*
 * One lookup declaration of a file. Strings are owned.
 *
 * scope     "::"-joined scope the declaration appears in ("" global; the
 *           owning class path for fields, the function's scope for returns).
 *           Never NULL.
 * name      Introduced name; NULL for using-directives.
 * target    "::" path the declaration refers to. Never NULL.
 * line      First line the declaration is visible on.
 * end_line  Last line of the enclosing block.
 * local     Declared inside a function body (visible only in that range).
 */
typedef struct {
    char        *scope;
    char        *name;
    char        *target;
    uint32_t     line;
    uint32_t     end_line;
    CtxDeclKind  kind;
    bool         local;
} CtxLookupDecl;

/* Output buffer of one file extraction: symbol drafts plus reference sites. */
typedef struct {
    CtxSymbolDraft *symbols;
    uint32_t    symbol_count;
    uint32_t    symbol_cap;
    CtxRefSite *sites;
    uint32_t    site_count;
    uint32_t    site_cap;
    CtxLookupDecl *decls;
    uint32_t    decl_count;
    uint32_t    decl_cap;
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
    CtxLookupDecl  *decls;
    uint32_t        decl_count;
    struct CtxGraphFile **includes;   /* resolved direct #includes (cache) */
    uint32_t        include_count;
    uint64_t        include_epoch;    /* g->include_epoch the cache was built for, 0 = stale */
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

/* Non-local lookup declarations sharing one name. */
typedef struct CtxDeclEntry {
    char                 *name;
    const CtxLookupDecl **decls;
    const CtxGraphFile  **files;
    uint32_t              count;
    uint32_t              cap;
    UT_hash_handle        hh;
} CtxDeclEntry;

/* Resolved base classes of one class (from inheritance edges). */
typedef struct CtxBaseEntry {
    uint64_t        class_id;
    uint64_t       *base_ids;
    uint32_t        count;
    uint32_t        cap;
    UT_hash_handle  hh;
} CtxBaseEntry;

/* Files sharing one basename, used to map #include strings to files. */
typedef struct CtxBasenameEntry {
    char                 *basename;
    CtxGraphFile        **files;
    uint32_t              count;
    uint32_t              cap;
    UT_hash_handle        hh;
} CtxBasenameEntry;

typedef struct {
    CtxSymbol    *symbols;   /* by id */
    CtxEdgeEntry *edges;     /* by composite key */
    CtxGraphFile *files;     /* by path */
    CtxNameEntry *names;     /* by name */
    CtxDeclEntry *decls;     /* non-local lookup declarations by name */
    CtxBaseEntry *bases;     /* class id -> base class ids */
    CtxBasenameEntry *basenames; /* file basename -> files */
    uint64_t      version_seq;
    uint64_t      include_epoch;  /* bumped whenever the file set changes */
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
 * Appends a reference site to an extraction buffer, copying its strings.
 * Sites without to_name are ignored. Returns false on allocation failure.
 */
bool ctx_file_extract_add_site(CtxFileExtract *ex, const CtxSiteDraft *site);

/*
 * Appends a lookup declaration to an extraction buffer, copying its strings.
 *
 * kind      Declaration kind.
 * scope     Enclosing scope (NULL treated as "").
 * name      Introduced name; NULL/"" for using-directives.
 * target    Referenced path; declarations without one are ignored.
 * line      First visible line.
 * end_line  Last line of the enclosing block.
 * local     Declared inside a function body.
 */
bool ctx_file_extract_add_decl(CtxFileExtract *ex, CtxDeclKind kind, const char *scope,
                               const char *name, const char *target,
                               uint32_t line, uint32_t end_line, bool local);

/* Releases everything owned by an extraction buffer and zeroes it. */
void ctx_file_extract_free(CtxFileExtract *ex);

/*
 * True when a symbol lives under qualifier ("::" or "." separated): its scope
 * ends with it, or for Python its module path plus scope does
 * ("pkg.mod.Class" matches method Class.run in pkg/mod.py).
 */
bool ctx_symbol_in_scope(const CtxSymbol *s, const char *qualifier);

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
