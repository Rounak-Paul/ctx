# Name Resolution — C, C++, Python (2026-10-01)

Primary languages: C, C++, Python. Others (JS/TS/Go/Rust) use the same
machinery but non-strict, name-based fallbacks.

## Data (graph/graph.h)
- `CtxSymbol.scope`: "::"-joined namespaces/classes (raw). Python *effective*
  scope = module path (last ≤11 dirs + module; `__init__` = package) + scope.
  Anonymous C++ namespaces add `CTX_ANONYMOUS_SCOPE` (skipped when matching).
- `CtxRefSite`: `to_name`, `to_scope` (explicit qualifier, C++ `ns::`,
  Python module/class path), `member`, `recv` (type expression), `arg_types`
  (';'-joined type expressions, C++ ADL).
- Type expressions (`CTX_TYPE_STEP_SEP`): head `T<path>` declared type, `S`
  this/self, `B` super(), `V<name>` implicit C++ member, `C<path>` call result;
  steps `|f<field>`, `|m<method>`. Evaluated at resolution (`eval_type`).
- `CtxLookupDecl` (store table `decls`): USING_NAMESPACE, USING,
  NAMESPACE_ALIAS, TYPE_ALIAS, FIELD (scope = raw class path, target = expr),
  RETURN (scope = function scope, target = expr). Python imports map to
  NAMESPACE_ALIAS (`import a.b` → a, `import a as b`), USING (`from m import f
  as g`), USING_NAMESPACE (`from m import *`); relative dots stripped.
- Store schema v6, `CTX_SEMANTIC_INDEX_VERSION` 12.

## Extractor (extractor/extractor.c)
- Walk tracks enclosing fn/scope, `fn_scope`, `in_method`,
  `fn_return_known`, typed variables (`LocalVar`, restored on fn exit;
  file-level declarations persist as globals) and parent nodes. Never call
  `ts_node_parent` per node (it walks from the root).
- C/C++: `record_lookup_decl` (using/alias/typedef), `record_declaration`
  (locals/params/globals, struct/class FIELDs incl. C, prototype RETURNs);
  function definitions record RETURN from their type. `typedef struct {..} X`
  names the struct X. Smart pointers/optional/make_unique unwrap to T.
- Python: `record_python` (imports, typed params/annotations, assignments,
  `self.x = …` fields, class attributes, return annotations or first typed
  `return`, binding targets of for/comprehension/with/walrus/unpacking).
  Attribute calls on values (self, locals, call results) are member sites;
  on module/class paths they become qualified sites. Module/class-level
  assignments are VARIABLE symbols. Bases come from `superclasses`.
- `classify_target` is the single call/member classifier.

## Resolution (graph/graph.c)
- `FileLookup` (per file per pass, lazy): transitive include closure
  (cached direct includes per file, invalidated by `include_epoch`), sorted
  closure paths/stems, own alias decls, closure using-directives, scratch
  `SiteLookup`s for nested evaluation (`CTX_EVAL_DEPTH`).
- `site_lookup_init`: expand aliases/imports (`expand_aliases`; "anchored"
  when the head is a declared name), retry unexpanded if nothing resolves.
- Field/return expressions evaluate in their declaring symbol's file and
  scope (`eval_in_context` swaps `fl->file`).
- `scope_score`:
  - qualified: suffix of effective scope 400(+near) / base of qualifying
    class 390; strict (reject others) for C++ and anchored Python.
  - member: C never binds (function-pointer fields); resolved receiver →
    class chain only (C++ rejects others); unknown receiver → type members.
  - unqualified: enclosing scopes, caller's bases, using-directives/star
    imports 190, ADL 180; Python rejects anything else (builtins stay
    unresolved).
- Locality: same file 100, include-reachable (closure or header stem
  sibling, C/C++) 60, same dir 40; definitions +20; kind rank.
- Incremental: affected names = old/new symbol + decl names; sites re-resolve
  when to_name/qualifier/recv/arg_types mention one; namespace-scope
  using-directives re-resolve all transitive includers.
- Inheritance sites resolve first (`g->bases` mirrors INHERITS edges).

## Known limits
Template-dependent receivers (`T obj`), untyped Python values (duck-typed
fallback to any type member), C++ overload selection by argument types,
Python dynamic attributes/`getattr`.

## Verification
- ctest: `cpp_scope_smoke`, `c_resolution_smoke`, `python_resolution_smoke`
  (harness `tests/resolution_harness.py`), plus existing smoke tests.
- llama.cpp: cold 23.3s, warm load 2.0s; full repo w/ vendors 66s.
