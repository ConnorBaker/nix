# Inventory — Shard 19: C bindings + misc

This shard catalogs the C ABI surface of every Nix library plus two small support binaries.
Files: libutil-c, libstore-c, libexpr-c, libfetchers-c, libflake-c, libmain-c, the clang-tidy plugin
skeleton, and the `nswrapper` user-namespace helper.

Recurring conventions across these libraries:

- All public headers wrap declarations in `extern "C"` (guarded by `__cplusplus`) and bracket the ABI
  with `// cffi start` / `// cffi end` markers for downstream binding generators. The fetchers and
  flake headers omit the closing `// cffi end` marker.
- All implementation files re-open `extern "C" { ... }` even though they are `.cc`, because the
  symbols must have C linkage.
- `*_internal.h` files are wrapped in `extern "C"`; `*_internal.hh` files are plain C++ (no
  `extern "C"`). Both define the opaque structs that the public headers forward-declare; they are
  the bridge between C handles and the C++ types they wrap (often via `nix::ref<T>`).
- Almost every fallible function takes a `nix_c_context * context` first arg, clears it on entry
  (`context->last_err_code = NIX_OK`, or `nix_clear_err(context)` in libflake-c), and ends a `try`
  block with the `NIXC_CATCH_ERRS{,_RES,_NULL}` macro family from `nix_api_util_internal.h`.

---

## File: src/libutil-c/nix_api_util.cc

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block wrapping every definition.

### Structs / enums (and what C++ types they wrap)
- None defined here (the opaque `nix_c_context` is defined in the internal header).

### Exported functions (nix_*)
- `nix_c_context * nix_c_context_create()` — heap-allocate a fresh `nix_c_context`; returns NULL on bad_alloc.
- `void nix_c_context_free(nix_c_context * context)` — `delete` the context (does not fail).
- `nix_err nix_context_error(nix_c_context * context)` — internal: rethrow the in-flight exception, capture its message/info/demangled name and code into the context (or rethrow if context is NULL).
- `nix_err nix_set_err_msg(nix_c_context * context, nix_err err, const char * msg)` — store an error code+message into the context (throws `nix::Error` if context is NULL).
- `void nix_clear_err(nix_c_context * context)` — set `last_err_code` back to `NIX_OK` if context is non-NULL.
- `const char * nix_version_get()` — return the compiled-in `PACKAGE_VERSION` string.
- `nix_err nix_setting_get(nix_c_context * context, const char * key, nix_get_string_callback callback, void * user_data)` — look up a setting in `nix::globalConfig` and stream its value to the callback; `NIX_ERR_KEY` if missing.
- `nix_err nix_setting_set(nix_c_context * context, const char * key, const char * value)` — set a key on `nix::globalConfig`; returns `NIX_ERR_KEY` if unknown.
- `nix_err nix_libutil_init(nix_c_context * context)` — call `nix::initLibUtil()`.
- `const char * nix_err_msg(nix_c_context * context, const nix_c_context * read_context, unsigned int * n)` — return the last error string (and optionally length) from `read_context`; on missing message records "No error message" into `context` and returns NULL.
- `nix_err nix_err_name(nix_c_context * context, const nix_c_context * read_context, nix_get_string_callback callback, void * user_data)` — stream the (demangled) class name of the last `nix::Error`; rejects when last error wasn't a `nix::Error`.
- `nix_err nix_err_info_msg(nix_c_context * context, const nix_c_context * read_context, nix_get_string_callback callback, void * user_data)` — stream the `ErrorInfo::msg` of the last `nix::Error`; rejects when last error wasn't a `nix::Error`.
- `nix_err nix_err_code(const nix_c_context * read_context)` — return the stored `last_err_code`.
- `nix_err nix_set_verbosity(nix_c_context * context, nix_verbosity level)` — write `nix::verbosity` after range-checking against `NIX_LVL_ERROR`/`NIX_LVL_VOMIT`.

### Internal helpers / globals
- `nix_err call_nix_get_string_callback(const std::string_view str, nix_get_string_callback callback, void * user_data)` — package a `string_view` into the C `(start,n,user_data)` calling convention; always returns `NIX_OK`. Declared in `nix_api_util_internal.h`.

### Type aliases
- None.

### Macros
- None defined here. `NIXC_CATCH_ERRS` (and friends) come from the internal header.

---

## File: src/libutil-c/nix_api_util.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block (CFFI-friendly markers `// cffi start` / `// cffi end`).

### Structs / enums (and what C++ types they wrap)
- `enum nix_err` — public error code enum (`NIX_OK=0`, `NIX_ERR_UNKNOWN=-1`, `NIX_ERR_OVERFLOW=-2`, `NIX_ERR_KEY=-3`, `NIX_ERR_NIX_ERROR=-4`, `NIX_ERR_RECOVERABLE=-5`).
- `enum nix_verbosity` — mirror of `nix::Verbosity` (`NIX_LVL_ERROR`, `NIX_LVL_WARN`, `NIX_LVL_NOTICE`, `NIX_LVL_INFO`, `NIX_LVL_TALKATIVE`, `NIX_LVL_CHATTY`, `NIX_LVL_DEBUG`, `NIX_LVL_VOMIT`).
- `struct nix_c_context` — opaque forward declaration; defined in `nix_api_util_internal.h`.

### Exported functions (nix_*)
- `nix_c_context * nix_c_context_create()` — allocate a context.
- `void nix_c_context_free(nix_c_context * context)` — free a context.
- `nix_err nix_libutil_init(nix_c_context * context)` — initialize libutil; idempotent.
- `nix_err nix_setting_get(nix_c_context * context, const char * key, nix_get_string_callback callback, void * user_data)` — fetch a global config setting.
- `nix_err nix_setting_set(nix_c_context * context, const char * key, const char * value)` — set a global config setting.
- `const char * nix_version_get()` — return the static version string.
- `const char * nix_err_msg(nix_c_context * context, const nix_c_context * ctx, unsigned int * n)` — last error message + length.
- `nix_err nix_err_info_msg(nix_c_context * context, const nix_c_context * read_context, nix_get_string_callback callback, void * user_data)` — last `nix::Error`'s `errorInfo.msg`.
- `nix_err nix_err_name(nix_c_context * context, const nix_c_context * read_context, nix_get_string_callback callback, void * user_data)` — class name of last `nix::Error`.
- `nix_err nix_err_code(const nix_c_context * read_context)` — last error code; never fails.
- `nix_err nix_set_err_msg(nix_c_context * context, nix_err err, const char * msg)` — set an error from a primop.
- `void nix_clear_err(nix_c_context * context)` — clear the stored error.
- `nix_err nix_set_verbosity(nix_c_context * context, nix_verbosity level)` — set global verbosity.

### Internal helpers / globals
- None.

### Type aliases
- `typedef enum nix_err nix_err` — enum tag-to-name alias.
- `typedef enum nix_verbosity nix_verbosity` — enum tag-to-name alias.
- `typedef struct nix_c_context nix_c_context` — opaque handle.
- `typedef void (*nix_get_string_callback)(const char * start, unsigned int n, void * user_data)` — borrowed-buffer callback signature used everywhere strings flow out of Nix.

### Macros
- None.

---

## File: src/libutil-c/nix_api_util_internal.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block.

### Structs / enums (and what C++ types they wrap)
- `struct nix_c_context` — concrete definition: `nix_err last_err_code = NIX_OK`, `std::optional<std::string> last_err`, `std::optional<nix::ErrorInfo> info`, `std::string name`. Wraps a `nix::ErrorInfo` plus a flat error code.

### Exported functions (nix_*)
- `nix_err nix_context_error(nix_c_context * context)` — declared here, implemented in `nix_api_util.cc`; converts the in-flight exception into context state.
- `nix_err call_nix_get_string_callback(const std::string_view str, nix_get_string_callback callback, void * user_data)` — declared here, implemented in `nix_api_util.cc`.

### Internal helpers / globals
- None.

### Type aliases
- None.

### Macros
- `NIXC_CATCH_ERRS` — `catch(...) { return nix_context_error(context); } return NIX_OK;` — closes a try-block.
- `NIXC_CATCH_ERRS_RES(def)` — variant that calls `nix_context_error(context)` then returns a default value on failure.
- `NIXC_CATCH_ERRS_NULL` — `NIXC_CATCH_ERRS_RES(nullptr)`.

---

## File: src/libstore-c/nix_api_store.cc

### Namespaces / extern "C" blocks
- Two `extern "C" { ... }` blocks separated by a single C++ template helper (`to_cpp_array`).

### Structs / enums (and what C++ types they wrap)
- None defined here (see `nix_api_store_internal.h`).

### Exported functions (nix_*)
- `nix_err nix_libstore_init(nix_c_context * context)` — call `nix::initLibStore()`.
- `nix_err nix_libstore_init_no_load_config(nix_c_context * context)` — call `nix::initLibStore(false)`.
- `Store * nix_store_open(nix_c_context * context, const char * uri, const char *** params)` — if uri is NULL/empty, open the default store; otherwise parse a `StoreReference`, splice in a NULL-terminated list of `(key,value)` parameter pairs, open the store, wrap in a `Store{nix::ref<nix::Store>}`.
- `void nix_store_free(Store * store)` — `delete` the C handle.
- `nix_err nix_store_get_uri(nix_c_context * context, Store * store, nix_get_string_callback callback, void * user_data)` — render the store reference (with params) and stream it.
- `nix_err nix_store_get_storedir(nix_c_context * context, Store * store, nix_get_string_callback callback, void * user_data)` — stream `store->storeDir`.
- `nix_err nix_store_get_version(nix_c_context * context, Store * store, nix_get_string_callback callback, void * user_data)` — stream `store->getVersion()` (empty if absent).
- `bool nix_store_is_valid_path(nix_c_context * context, Store * store, const StorePath * path)` — `Store::isValidPath`.
- `nix_err nix_store_real_path(nix_c_context * context, Store * store, StorePath * path, nix_get_string_callback callback, void * user_data)` — for `LocalFSStore` returns `toRealPath`, otherwise `printStorePath`.
- `StorePath * nix_store_parse_path(nix_c_context * context, Store * store, const char * path)` — parse a full `<storeDir>/<hash>-<name>` path into a `StorePath` C handle.
- `nix_err nix_store_get_fs_closure(nix_c_context * context, Store * store, const StorePath * store_path, bool flip_direction, bool include_outputs, bool include_derivers, void * userdata, void (*callback)(nix_c_context *, void *, const StorePath *))` — call `Store::computeFSClosure` and invoke the C callback for each path; aborts iteration on context error.
- `nix_err nix_store_realise(nix_c_context * context, Store * store, StorePath * path, void * userdata, void (*callback)(void *, const char *, const StorePath *))` — wrap a `StorePath` into a `DerivedPath::Built` with `OutputsSpec::All`, call `buildPathsWithResults` with `bmNormal`, throw on build error via `tryThrowBuildError`, then enumerate `(outputName, outPath)` from the success result to the callback.
- `void nix_store_path_name(const StorePath * store_path, nix_get_string_callback callback, void * user_data)` — pass `store_path->path.name()` to the callback. Never fails (no try/catch, no context).
- `void nix_store_path_free(StorePath * sp)` — delete the handle.
- `void nix_derivation_free(nix_derivation * drv)` — delete the handle.
- `StorePath * nix_store_path_clone(const StorePath * p)` — copy-construct a new `StorePath`. Returns NULL on bad_alloc.
- `nix_err nix_store_path_hash(nix_c_context * context, const StorePath * store_path, nix_store_path_hash_part * hash_part_out)` — decode the path's hash from base32 into 20 raw bytes via `BaseNix32::decode`.
- `StorePath * nix_store_create_from_parts(nix_c_context * context, const nix_store_path_hash_part * hash, const char * name, size_t name_len)` — re-encode 20 bytes to base32 via `BaseNix32::encode`, concatenate `<hash>-<name>`, construct a `nix::StorePath`.
- `nix_derivation * nix_derivation_clone(const nix_derivation * d)` — copy-construct, NULL on failure.
- `nix_derivation * nix_derivation_from_json(nix_c_context * context, Store * store, const char * json)` — call `nix::Derivation::parseJsonAndValidate` on `nlohmann::json::parse(json)`.
- `nix_err nix_derivation_to_json(nix_c_context * context, const nix_derivation * drv, nix_get_string_callback callback, void * userdata)` — serialize the derivation as JSON via `nlohmann::json` cast and call the callback (only if non-NULL).
- `StorePath * nix_add_derivation(nix_c_context * context, Store * store, nix_derivation * derivation)` — `writeDerivation` (or `computeStorePath` in `settings.readOnlyMode`); back-compat shortcut explicitly retained per source comment.
- `nix_err nix_store_copy_closure(nix_c_context * context, Store * srcStore, Store * dstStore, StorePath * path)` — single-element `RealisedPath::Set` wrapper around `nix::copyClosure`.
- `nix_derivation * nix_store_drv_from_store_path(nix_c_context * context, Store * store, const StorePath * path)` — call `Store::derivationFromPath`.
- `StorePath * nix_store_query_path_from_hash_part(nix_c_context * context, Store * store, const char * hash)` — `Store::queryPathFromHashPart`; NULL if absent.
- `nix_err nix_store_copy_path(nix_c_context * context, Store * srcStore, Store * dstStore, const StorePath * path, bool repair, bool checkSigs)` — `nix::copyStorePath` with explicit null-pointer checks for the three pointer args (returns `NIX_ERR_UNKNOWN` on null).

### Internal helpers / globals
- `template<size_t S> static auto to_cpp_array(const uint8_t (&r)[S])` — `reinterpret_cast` a C array to `std::array<std::byte, S>` so `BaseNix32::encode` can take a `span<const byte>`.

### Type aliases
- None.

### Macros
- None.

---

## File: src/libstore-c/nix_api_store.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block (`// cffi start`/`// cffi end`).

### Structs / enums (and what C++ types they wrap)
- `struct Store` — opaque forward declaration; defined in `nix_api_store_internal.h` to wrap `nix::ref<nix::Store>`.

### Exported functions (nix_*)
- `nix_err nix_libstore_init(nix_c_context * context)` — initialize libstore.
- `nix_err nix_libstore_init_no_load_config(nix_c_context * context)` — initialize libstore without loading nix.conf.
- `Store * nix_store_open(nix_c_context * context, const char * uri, const char *** params)` — open a store.
- `void nix_store_free(Store * store)` — close/free a store.
- `nix_err nix_store_get_uri(nix_c_context * context, Store * store, nix_get_string_callback callback, void * user_data)` — get rendered URI.
- `nix_err nix_store_get_storedir(nix_c_context * context, Store * store, nix_get_string_callback callback, void * user_data)` — get storeDir.
- `StorePath * nix_store_parse_path(nix_c_context * context, Store * store, const char * path)` — parse a `<storeDir>/<hash>-<name>` path.
- `bool nix_store_is_valid_path(nix_c_context * context, Store * store, const StorePath * path)` — check validity.
- `nix_err nix_store_real_path(nix_c_context * context, Store * store, StorePath * path, nix_get_string_callback callback, void * user_data)` — physical path on a (possibly-relocated) FS store.
- `nix_err nix_store_realise(nix_c_context * context, Store * store, StorePath * path, void * userdata, void (*callback)(void * userdata, const char * outname, const StorePath * out))` — build a path; callback per output.
- `nix_err nix_store_get_version(nix_c_context * context, Store * store, nix_get_string_callback callback, void * user_data)` — get store version, "" if unknown.
- `nix_derivation * nix_derivation_from_json(nix_c_context * context, Store * store, const char * json)` — parse a JSON-serialized derivation.
- `StorePath * nix_add_derivation(nix_c_context * context, Store * store, nix_derivation * derivation)` — write a derivation to the store.
- `nix_err nix_store_copy_closure(nix_c_context * context, Store * srcStore, Store * dstStore, StorePath * path)` — copy a path's closure between stores.
- `nix_err nix_store_get_fs_closure(nix_c_context * context, Store * store, const StorePath * store_path, bool flip_direction, bool include_outputs, bool include_derivers, void * userdata, void (*callback)(nix_c_context *, void *, const StorePath *))` — enumerate FS closure paths.
- `nix_derivation * nix_store_drv_from_store_path(nix_c_context * context, Store * store, const StorePath * path)` — derivation associated with a path.
- `StorePath * nix_store_query_path_from_hash_part(nix_c_context * context, Store * store, const char * hash)` — recover full path from a hash prefix.
- `nix_err nix_store_copy_path(nix_c_context * context, Store * srcStore, Store * dstStore, const StorePath * path, bool repair, bool checkSigs)` — copy a single path between stores.

### Internal helpers / globals
- None.

### Type aliases
- `typedef struct Store Store` — opaque handle (declared inside the `extern "C"` block as `typedef struct Store Store`).

### Macros
- None.

---

## File: src/libstore-c/nix_api_store_internal.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block.

### Structs / enums (and what C++ types they wrap)
- `struct Store { nix::ref<nix::Store> ptr; }` — wraps a counted ref to the C++ `nix::Store`.
- `struct StorePath { nix::StorePath path; }` — wraps `nix::StorePath` by value.
- `struct nix_derivation { nix::Derivation drv; }` — wraps `nix::Derivation` by value.

### Exported functions (nix_*)
- None (definitions only).

### Internal helpers / globals
- None.

### Type aliases
- None.

### Macros
- None.

---

## File: src/libstore-c/nix_api_store/derivation.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block (`// cffi start`/`// cffi end`).

### Structs / enums (and what C++ types they wrap)
- `struct nix_derivation` — opaque forward decl (defined in `nix_api_store_internal.h`).

### Exported functions (nix_*)
- `nix_derivation * nix_derivation_clone(const nix_derivation * d)` — copy a derivation handle.
- `void nix_derivation_free(nix_derivation * drv)` — free a derivation.
- `nix_err nix_derivation_to_json(nix_c_context * context, const nix_derivation * drv, nix_get_string_callback callback, void * userdata)` — JSON-serialize the derivation.

### Internal helpers / globals
- None.

### Type aliases
- `typedef struct nix_derivation nix_derivation` — opaque handle.

### Macros
- None.

---

## File: src/libstore-c/nix_api_store/store_path.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block (`// cffi start`/`// cffi end`).

### Structs / enums (and what C++ types they wrap)
- `struct StorePath` — opaque forward decl (defined in `nix_api_store_internal.h`).
- `struct nix_store_path_hash_part { uint8_t bytes[20]; }` — concrete; the 20 raw bytes underlying a base32-encoded store-path hash.

### Exported functions (nix_*)
- `StorePath * nix_store_path_clone(const StorePath * p)` — copy a `StorePath`.
- `void nix_store_path_free(StorePath * p)` — free a `StorePath`.
- `void nix_store_path_name(const StorePath * store_path, nix_get_string_callback callback, void * user_data)` — get the `<name>` portion.
- `nix_err nix_store_path_hash(nix_c_context * context, const StorePath * store_path, nix_store_path_hash_part * hash_part_out)` — get the 20 raw hash bytes.
- `StorePath * nix_store_create_from_parts(nix_c_context * context, const nix_store_path_hash_part * hash, const char name[/*name_len*/], size_t name_len)` — build a `StorePath` from raw hash bytes plus a name.

### Internal helpers / globals
- None.

### Type aliases
- `typedef struct StorePath StorePath` — opaque handle.
- `typedef struct nix_store_path_hash_part { uint8_t bytes[20]; } nix_store_path_hash_part` — value type for the 20-byte hash.

### Macros
- None.

---

## File: src/libexpr-c/nix_api_expr.cc

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block.

### Structs / enums (and what C++ types they wrap)
- None defined here (see `nix_api_expr_internal.h`).

### Exported functions (nix_*)
- `nix_err nix_libexpr_init(nix_c_context * context)` — chains `nix_libutil_init` then `nix_libstore_init`, then calls `nix::initGC()` in a try block.
- `nix_err nix_expr_eval_from_string(nix_c_context * context, EvalState * state, const char * expr, const char * path, nix_value * value)` — `parseExprFromString`, `eval`, then `forceValue` at `noPos`.
- `nix_err nix_value_call(nix_c_context * context, EvalState * state, Value * fn, nix_value * arg, nix_value * value)` — single-argument function call (`callFunction` then `forceValue`); note `Value *` (deprecated alias for `nix_value *`) in the parameter list.
- `nix_err nix_value_call_multi(nix_c_context * context, EvalState * state, nix_value * fn, size_t nargs, nix_value ** args, nix_value * value)` — multi-argument call by collecting `nix::Value *` pointers into a `std::vector` and passing as a span.
- `nix_err nix_value_force(nix_c_context * context, EvalState * state, nix_value * value)` — call `forceValue` at `noPos`.
- `nix_err nix_value_force_deep(nix_c_context * context, EvalState * state, nix_value * value)` — call `forceValueDeep`.
- `nix_eval_state_builder * nix_eval_state_builder_new(nix_c_context * context, Store * store)` — allocate a new builder with a fresh `nix::ref<bool>` for `readOnlyMode`, default `EvalSettings` (referencing that bool) and default `fetchers::Settings`.
- `void nix_eval_state_builder_free(nix_eval_state_builder * builder)` — `delete` the builder.
- `nix_err nix_eval_state_builder_load(nix_c_context * context, nix_eval_state_builder * builder)` — re-points `settings.readOnlyMode` at the global `nix::settings.readOnlyMode`, then calls `loadConfFile` on both `settings` and `fetchSettings`.
- `nix_err nix_eval_state_builder_set_lookup_path(nix_c_context * context, nix_eval_state_builder * builder, const char ** lookupPath_c)` — parse a NULL-terminated string array into `LookupPath`.
- `EvalState * nix_eval_state_build(nix_c_context * context, nix_eval_state_builder * builder)` — move builder fields into owned `unique_ptr`s, construct a `shared_ptr<nix::EvalState>`, placement-new an `EvalState` C handle (with explicit aligned `operator new`) keeping the unique_ptr/shared_ptr alive.
- `EvalState * nix_state_create(nix_c_context * context, const char ** lookupPath_c, Store * store)` — convenience: builder_new + load + set_lookup_path + build + free, with NULL/error short-circuits.
- `void nix_state_free(EvalState * state)` — explicit `~EvalState()` + aligned `operator delete` (the struct holds C++ unique_ptrs but was placement-new'd).
- `nix_err nix_gc_incref(nix_c_context * context, const void * p)` — under `NIX_USE_BOEHMGC`: increment a refcount in the global `nix_refcounts` concurrent flat map. Without GC, no-op.
- `nix_err nix_gc_decref(nix_c_context * context, const void * p)` — under Boehm GC: decrement and erase when reaching zero; throws `std::runtime_error` if the object is unknown. Without GC, no-op.
- `void nix_gc_now()` — under Boehm GC: `GC_gcollect()`. Without GC, no-op.
- `nix_err nix_value_incref(nix_c_context * context, nix_value * x)` — typed alias forwarding to `nix_gc_incref`.
- `nix_err nix_value_decref(nix_c_context * context, nix_value * x)` — typed alias forwarding to `nix_gc_decref`.
- `void nix_gc_register_finalizer(void * obj, void * cd, void (*finalizer)(void * obj, void * cd))` — under Boehm GC: `GC_REGISTER_FINALIZER`. Without GC, body is empty.

### Internal helpers / globals
- `boost::concurrent_flat_map<const void *, unsigned int, std::hash<const void *>, std::equal_to<const void *>, traceable_allocator<std::pair<const void * const, unsigned int>>> nix_refcounts` — global refcount table when `NIX_USE_BOEHMGC`.

### Type aliases
- None.

### Macros
- None directly defined; uses `NIX_USE_BOEHMGC` config macro.

---

## File: src/libexpr-c/nix_api_expr.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block (`// cffi start`/`// cffi end`).

### Structs / enums (and what C++ types they wrap)
- `struct nix_eval_state_builder` — opaque forward decl.
- `struct EvalState` — opaque forward decl, wraps `nix::EvalState`.
- `struct nix_value` — opaque forward decl, wraps `nix::Value *` plus `nix::EvalMemory *`.

### Exported functions (nix_*)
- `nix_err nix_libexpr_init(nix_c_context * context)` — initialize the language evaluator.
- `nix_err nix_expr_eval_from_string(nix_c_context * context, EvalState * state, const char * expr, const char * path, nix_value * value)` — parse+evaluate.
- `nix_err nix_value_call(nix_c_context * context, EvalState * state, nix_value * fn, nix_value * arg, nix_value * value)` — single-arg apply.
- `nix_err nix_value_call_multi(nix_c_context * context, EvalState * state, nix_value * fn, size_t nargs, nix_value ** args, nix_value * value)` — multi-arg apply.
- `nix_err nix_value_force(nix_c_context * context, EvalState * state, nix_value * value)` — force a value.
- `nix_err nix_value_force_deep(nix_c_context * context, EvalState * state, nix_value * value)` — recursively force.
- `nix_eval_state_builder * nix_eval_state_builder_new(nix_c_context * context, Store * store)` — fresh builder.
- `nix_err nix_eval_state_builder_load(nix_c_context * context, nix_eval_state_builder * builder)` — read settings from env + nix.conf.
- `nix_err nix_eval_state_builder_set_lookup_path(nix_c_context * context, nix_eval_state_builder * builder, const char ** lookupPath)` — set NIX_PATH analog.
- `EvalState * nix_eval_state_build(nix_c_context * context, nix_eval_state_builder * builder)` — finalize a builder into an `EvalState`.
- `void nix_eval_state_builder_free(nix_eval_state_builder * builder)` — free a builder.
- `EvalState * nix_state_create(nix_c_context * context, const char ** lookupPath, Store * store)` — convenience builder pipeline.
- `void nix_state_free(EvalState * state)` — destroy an `EvalState`.
- `nix_err nix_gc_incref(nix_c_context * context, const void * object)` — refcount increment.
- `nix_err nix_gc_decref(nix_c_context * context, const void * object)` — refcount decrement (deprecated for typed variants per Doxygen).
- `void nix_gc_now()` — manual GC trigger.
- `void nix_gc_register_finalizer(void * obj, void * cd, void (*finalizer)(void * obj, void * cd))` — register a Boehm finalizer.

### Internal helpers / globals
- None.

### Type aliases
- `typedef struct nix_eval_state_builder nix_eval_state_builder` — opaque handle.
- `typedef struct EvalState EvalState` — opaque handle for `nix::EvalState`.
- `typedef struct nix_value nix_value` — opaque handle.
- `NIX_DEPRECATED("use nix_value instead") typedef nix_value Value` — back-compat alias.

### Macros
- `__has_c_attribute(x)` — fallback definition to 0 when not supported.
- `NIX_DEPRECATED(msg)` — expands to `[[deprecated(msg)]]` if `__has_c_attribute(deprecated)`, else nothing.
- `NIX_VALUE_CALL(context, state, value, fn, ...)` — variadic do-while wrapping `nix_value_call_multi` so callers can pass arguments as a comma list.

---

## File: src/libexpr-c/nix_api_expr_internal.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block.

### Structs / enums (and what C++ types they wrap)
- `struct nix_eval_state_builder` — `nix::ref<nix::Store> store`, `nix::EvalSettings settings`, `nix::fetchers::Settings fetchSettings`, `nix::LookupPath lookupPath`, `nix::ref<bool> readOnlyMode`.
- `struct EvalState` — `nix::EvalState & state` reference plus three optionally-owned smart pointers: `unique_ptr<nix::fetchers::Settings> ownedFetchSettings`, `unique_ptr<nix::EvalSettings> ownedSettings`, `shared_ptr<nix::EvalState> ownedState` (null for temporary wrappers used inside primop callbacks).
- `struct BindingsBuilder { nix::BindingsBuilder builder; }` — wraps `nix::BindingsBuilder`.
- `struct ListBuilder { nix::ListBuilder builder; }` — wraps `nix::ListBuilder`.
- `struct nix_value` — `nix::Value * value` plus `nix::EvalMemory * mem` (the comment notes this is a stable-ABI workaround so `EvalMemory` is reachable from later operations).
- `struct nix_string_return { std::string str; }` — buffer for primops/external values to return strings.
- `struct nix_printer { std::ostream & s; }` — wraps an output stream for `nix_external_print`.
- `struct nix_string_context { nix::NixStringContext & ctx; }` — wraps a string-context accumulator.
- `struct nix_realised_string { std::string str; std::vector<StorePath> storePaths; }` — output of `nix_string_realise`.

### Exported functions (nix_*)
- None (struct-only).

### Internal helpers / globals
- None.

### Type aliases
- None.

### Macros
- None.

---

## File: src/libexpr-c/nix_api_external.cc

### Namespaces / extern "C" blocks
- Two `extern "C" { ... }` blocks bracketing the C++ class definition.

### Structs / enums (and what C++ types they wrap)
- `class NixCExternalValue : public nix::ExternalValueBase` — internal: holds a reference to a user-supplied `NixCExternalValueDesc` plus a `void * v` payload, exposes `get_ptr()`, and forwards C++ `ExternalValueBase` virtuals (`print`, `showType`, `typeOf`, `coerceToString`, `operator==`, `printValueAsJSON`, `printValueAsXML`, dtor) to the C callbacks. Falls back to `ExternalValueBase` defaults when the descriptor's pointer is null or the callback returns an empty string. Constructs a temporary `EvalState` wrapper (`EvalState wrapper{state};`) for the JSON/XML paths.

### Exported functions (nix_*)
- `void nix_set_string_return(nix_string_return * str, const char * c)` — copy a C string into the return buffer (no context, never fails).
- `nix_err nix_external_print(nix_c_context * context, nix_printer * printer, const char * c)` — write a string to the wrapped stream.
- `nix_err nix_external_add_string_context(nix_c_context * context, nix_string_context * ctx, const char * c)` — `NixStringContextElem::parse` then insert into the wrapped accumulator.
- `ExternalValue * nix_create_external_value(nix_c_context * context, NixCExternalValueDesc * desc, void * v)` — heap-allocate (under GC if `NIX_USE_BOEHMGC`) a `NixCExternalValue`, `nix_gc_incref` and return cast to `ExternalValue *`.
- `void * nix_get_external_value_content(nix_c_context * context, ExternalValue * b)` — `dynamic_cast` back to `NixCExternalValue` and return its `void * v`; NULL if not from this API.

### Internal helpers / globals
- None outside the class.

### Type aliases
- None.

### Macros
- None.

---

## File: src/libexpr-c/nix_api_external.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block (`// cffi start`/`// cffi end`).

### Structs / enums (and what C++ types they wrap)
- `struct nix_string_return` — opaque forward decl (defined in `nix_api_expr_internal.h`).
- `struct nix_printer` — opaque forward decl.
- `struct nix_string_context` — opaque forward decl.
- `struct NixCExternalValueDesc` — concrete descriptor of function pointers a host language fills in to teach Nix how to handle a foreign value: `print`, `showType`, `typeOf`, `coerceToString`, `equal`, `printValueAsJSON`, `printValueAsXML`. Several pointers are documented as optional; the C++ class falls back to `ExternalValueBase` defaults if NULL.

### Exported functions (nix_*)
- `void nix_set_string_return(nix_string_return * str, const char * c)` — set a return string buffer.
- `nix_err nix_external_print(nix_c_context * context, nix_printer * printer, const char * str)` — print to a `nix_printer`.
- `nix_err nix_external_add_string_context(nix_c_context * context, nix_string_context * string_context, const char * c)` — append a string-context element.
- `ExternalValue * nix_create_external_value(nix_c_context * context, NixCExternalValueDesc * desc, void * v)` — wrap a host pointer in a Nix `ExternalValue`.
- `void * nix_get_external_value_content(nix_c_context * context, ExternalValue * b)` — recover the host pointer from an `ExternalValue` (NULL if foreign).

### Internal helpers / globals
- None.

### Type aliases
- `typedef struct nix_string_return nix_string_return`.
- `typedef struct nix_printer nix_printer`.
- `typedef struct nix_string_context nix_string_context`.
- `typedef struct NixCExternalValueDesc { ... } NixCExternalValueDesc` — concrete function-pointer table.

### Macros
- None.

---

## File: src/libexpr-c/nix_api_value.cc

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block surrounding all exported definitions; the helper validators and `nix_c_primop_wrapper` are at file scope (C++ linkage internal).

### Structs / enums (and what C++ types they wrap)
- None defined here.

### Exported functions (nix_*)
- `PrimOp * nix_alloc_primop(nix_c_context * context, PrimOpFun fun, int arity, const char * name, const char ** args, const char * doc, void * user_data)` — allocate a `nix::PrimOp` (under GC if `NIX_USE_BOEHMGC`) wired to `nix_c_primop_wrapper` via `std::bind`, append the NULL-terminated `args` strings, increment its GC refcount, return cast to opaque `PrimOp *`.
- `nix_err nix_register_primop(nix_c_context * context, PrimOp * primOp)` — construct a temporary `nix::RegisterPrimOp` from a moved-from `nix::PrimOp` to add the primop globally (registry side effect of `RegisterPrimOp`'s ctor).
- `nix_value * nix_alloc_value(nix_c_context * context, EvalState * state)` — allocate a `nix::Value` via `state.allocValue()` and a managed `nix_value` wrapper via `new_nix_value`.
- `ValueType nix_get_type(nix_c_context * context, const nix_value * value)` — switch over `nix::Value::type()` to map to `NIX_TYPE_*` (covers `nThunk`, `nFailed`, `nInt`, `nFloat`, `nBool`, `nString`, `nPath`, `nNull`, `nAttrs`, `nList`, `nFunction`, `nExternal`).
- `const char * nix_get_typename(nix_c_context * context, const nix_value * value)` — `nix::showType` -> `strdup`.
- `bool nix_get_bool(nix_c_context * context, const nix_value * value)` — return `value->boolean()` after asserting type is `nBool`.
- `nix_err nix_get_string(nix_c_context * context, const nix_value * value, nix_get_string_callback callback, void * user_data)` — stream `string_view`.
- `const char * nix_get_path_string(nix_c_context * context, const nix_value * value)` — return `value->pathStr()` after asserting type is `nPath`.
- `unsigned int nix_get_list_size(nix_c_context * context, const nix_value * value)` — `listSize`.
- `unsigned int nix_get_attrs_size(nix_c_context * context, const nix_value * value)` — `attrs->size()`.
- `double nix_get_float(nix_c_context * context, const nix_value * value)` — `fpoint`.
- `int64_t nix_get_int(nix_c_context * context, const nix_value * value)` — `integer().value`.
- `ExternalValue * nix_get_external(nix_c_context * context, nix_value * value)` — return `external()` cast to `ExternalValue *`; uses `check_value_out` (note: this is a quirk; signature accepts non-const `nix_value *`).
- `nix_value * nix_get_list_byidx(nix_c_context * context, const nix_value * value, EvalState * state, unsigned int ix)` — bounds-check (returns `NIX_ERR_KEY` on OOB), index via `listView`, null-check the element, force, then wrap.
- `nix_value * nix_get_list_byidx_lazy(nix_c_context * context, const nix_value * value, EvalState * state, unsigned int ix)` — same as above but skips the `forceValue` call (and the null-pointer check on the element pointer).
- `nix_value * nix_get_attr_byname(nix_c_context * context, const nix_value * value, EvalState * state, const char * name)` — symbol-create lookup, force, wrap; `NIX_ERR_KEY` if missing.
- `nix_value * nix_get_attr_byname_lazy(nix_c_context * context, const nix_value * value, EvalState * state, const char * name)` — same as above without the `forceValue` call.
- `bool nix_has_attr_byname(nix_c_context * context, const nix_value * value, EvalState * state, const char * name)` — symbol-create lookup; returns true if attr exists.
- `nix_value * nix_get_attr_byidx(nix_c_context * context, nix_value * value, EvalState * state, unsigned int i, const char ** name)` — `collapse_attrset_layer_chain_if_needed`, return ith attr (forcing) and write its name into `*name`; `NIX_ERR_KEY` on OOB.
- `nix_value * nix_get_attr_byidx_lazy(nix_c_context * context, nix_value * value, EvalState * state, unsigned int i, const char ** name)` — same as above without the `forceValue` call.
- `const char * nix_get_attr_name_byidx(nix_c_context * context, nix_value * value, EvalState * state, unsigned int i)` — `collapse_attrset_layer_chain_if_needed`, return the symbol's `c_str()` for the ith attr.
- `nix_err nix_init_bool(nix_c_context * context, nix_value * value, bool b)` — `mkBool` on an output value.
- `nix_err nix_init_string(nix_c_context * context, nix_value * value, const char * str)` — `mkString` using `value->mem` (TODO: string context per source comment).
- `nix_err nix_init_path_string(nix_c_context * context, EvalState * s, nix_value * value, const char * str)` — `mkPath` against `state.rootPath(CanonPath(str))` using `state.mem`.
- `nix_err nix_init_float(nix_c_context * context, nix_value * value, double d)` — `mkFloat`.
- `nix_err nix_init_int(nix_c_context * context, nix_value * value, int64_t i)` — `mkInt`.
- `nix_err nix_init_null(nix_c_context * context, nix_value * value)` — `mkNull`.
- `nix_err nix_init_apply(nix_c_context * context, nix_value * value, nix_value * fn, nix_value * arg)` — `mkApp` (creates a thunk); uses `check_value_not_null` (not `check_value_out`) on all three args.
- `nix_err nix_init_external(nix_c_context * context, nix_value * value, ExternalValue * val)` — `mkExternal` after casting to `nix::ExternalValueBase *`.
- `ListBuilder * nix_make_list_builder(nix_c_context * context, EvalState * state, size_t capacity)` — allocate (`(NoGC)` placement under Boehm) a `ListBuilder` wrapping `state.buildList(capacity)`.
- `nix_err nix_list_builder_insert(nix_c_context * context, ListBuilder * list_builder, unsigned int index, nix_value * value)` — store `&value->value` at index.
- `void nix_list_builder_free(ListBuilder * list_builder)` — `GC_FREE` under Boehm, otherwise `delete`.
- `nix_err nix_make_list(nix_c_context * context, ListBuilder * list_builder, nix_value * value)` — `mkList`.
- `nix_err nix_init_primop(nix_c_context * context, nix_value * value, PrimOp * p)` — `mkPrimOp` after casting.
- `nix_err nix_copy_value(nix_c_context * context, nix_value * value, const nix_value * source)` — copy-assign `nix::Value` (after `check_value_out` on dst, `check_value_in` on src).
- `nix_err nix_make_attrs(nix_c_context * context, nix_value * value, BindingsBuilder * b)` — `mkAttrs`.
- `BindingsBuilder * nix_make_bindings_builder(nix_c_context * context, EvalState * state, size_t capacity)` — allocate (`(NoGC)` placement under Boehm) a `BindingsBuilder` wrapping `state.buildBindings(capacity)`.
- `nix_err nix_bindings_builder_insert(nix_c_context * context, BindingsBuilder * bb, const char * name, nix_value * value)` — `symbols.create(name)`, `builder.insert`.
- `void nix_bindings_builder_free(BindingsBuilder * bb)` — `GC_FREE` under Boehm, otherwise `delete` (cast to `nix::BindingsBuilder *` first).
- `nix_realised_string * nix_string_realise(nix_c_context * context, EvalState * state, nix_value * value, bool isIFD)` — call `state.realiseString`, copy each `StorePath` into a `std::vector<StorePath>` and return a heap `nix_realised_string`.
- `void nix_realised_string_free(nix_realised_string * s)` — `delete`.
- `size_t nix_realised_string_get_buffer_size(nix_realised_string * s)` — `str.size()`.
- `const char * nix_realised_string_get_buffer_start(nix_realised_string * s)` — `str.data()`.
- `size_t nix_realised_string_get_store_path_count(nix_realised_string * s)` — `storePaths.size()`.
- `const StorePath * nix_realised_string_get_store_path(nix_realised_string * s, size_t i)` — `&storePaths[i]`.

### Internal helpers / globals
- `static const nix::Value & check_value_not_null(const nix_value * value)` — null-pointer guard, returns the underlying value.
- `static nix::Value & check_value_not_null(nix_value * value)` — non-const overload.
- `static const nix::Value & check_value_in(const nix_value * value)` — null + `isValid` guard for inputs.
- `static nix::Value & check_value_in(nix_value * value)` — non-const overload.
- `static nix::Value & check_value_out(nix_value * value)` — null + `!isValid` guard for output slots (Nix values are immutable once initialized).
- `static nix_value * new_nix_value(nix::Value * v, nix::EvalMemory & mem)` — placement-new a `nix_value` inside `EvalMemory` (`mem.allocBytes(sizeof(nix_value))`) and `nix_gc_incref` it.
- `static void nix_c_primop_wrapper(PrimOpFun f, void * userdata, int arity, nix::EvalState & state, const nix::PosIdx pos, nix::Value ** args, nix::Value & v)` — bridges C primop callbacks to the `nix::PrimOp::impl` signature; allocates a temporary `nix::Value vTmp` so the original thunk can be retried (per the in-source explanation), wraps args, calls into C with a temporary `EvalState wrapper{state}`, classifies error code (`NIX_ERR_RECOVERABLE` -> `RecoverableEvalError`, otherwise `EvalError`), refuses uninitialized return values and refuses thunk return values, then assigns `v = vTmp`.
- `static void collapse_attrset_layer_chain_if_needed(nix::Value & v, EvalState * state)` — if `v.attrs()->isLayered()`, copy them into a flat `buildBindings` and `mkAttrs` the result so `byidx` operations have a stable ordering.

### Type aliases
- None.

### Macros
- None directly defined here.

---

## File: src/libexpr-c/nix_api_value.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block (`// cffi start`/`// cffi end`).

### Structs / enums (and what C++ types they wrap)
- `enum ValueType` (anonymous enum aliased as `ValueType` via `typedef enum { ... } ValueType`) — `NIX_TYPE_THUNK`, `NIX_TYPE_INT`, `NIX_TYPE_FLOAT`, `NIX_TYPE_BOOL`, `NIX_TYPE_STRING`, `NIX_TYPE_PATH`, `NIX_TYPE_NULL`, `NIX_TYPE_ATTRS`, `NIX_TYPE_LIST`, `NIX_TYPE_FUNCTION`, `NIX_TYPE_EXTERNAL`, `NIX_TYPE_FAILED` — mirror of `nix::ValueType`.
- `struct nix_value` — opaque forward decl.
- `struct EvalState` — opaque forward decl.
- `struct BindingsBuilder` — opaque forward decl.
- `struct ListBuilder` — opaque forward decl.
- `struct PrimOp` — opaque forward decl.
- `struct ExternalValue` — opaque forward decl.
- `struct nix_realised_string` — opaque forward decl.

### Exported functions (nix_*)
- `PrimOp * nix_alloc_primop(nix_c_context * context, PrimOpFun fun, int arity, const char * name, const char ** args, const char * doc, void * user_data)` — allocate a primop.
- `nix_err nix_register_primop(nix_c_context * context, PrimOp * primOp)` — register globally.
- `nix_value * nix_alloc_value(nix_c_context * context, EvalState * state)` — allocate a value.
- `nix_err nix_value_incref(nix_c_context * context, nix_value * value)` — refcount up.
- `nix_err nix_value_decref(nix_c_context * context, nix_value * value)` — refcount down.
- `ValueType nix_get_type(nix_c_context * context, const nix_value * value)` — discriminator.
- `const char * nix_get_typename(nix_c_context * context, const nix_value * value)` — type name (caller must `free`).
- `bool nix_get_bool(nix_c_context * context, const nix_value * value)` — extract boolean.
- `nix_err nix_get_string(nix_c_context * context, const nix_value * value, nix_get_string_callback callback, void * user_data)` — extract string (raw, pre-realise).
- `const char * nix_get_path_string(nix_c_context * context, const nix_value * value)` — extract path string.
- `unsigned int nix_get_list_size(nix_c_context * context, const nix_value * value)` — list size.
- `unsigned int nix_get_attrs_size(nix_c_context * context, const nix_value * value)` — attrset size.
- `double nix_get_float(nix_c_context * context, const nix_value * value)` — extract float.
- `int64_t nix_get_int(nix_c_context * context, const nix_value * value)` — extract int.
- `ExternalValue * nix_get_external(nix_c_context * context, nix_value * value)` — extract external.
- `nix_value * nix_get_list_byidx(nix_c_context * context, const nix_value * value, EvalState * state, unsigned int ix)` — list element + force.
- `nix_value * nix_get_list_byidx_lazy(nix_c_context * context, const nix_value * value, EvalState * state, unsigned int ix)` — list element without force.
- `nix_value * nix_get_attr_byname(nix_c_context * context, const nix_value * value, EvalState * state, const char * name)` — attr lookup + force.
- `nix_value * nix_get_attr_byname_lazy(nix_c_context * context, const nix_value * value, EvalState * state, const char * name)` — attr lookup without force.
- `bool nix_has_attr_byname(nix_c_context * context, const nix_value * value, EvalState * state, const char * name)` — attr existence test.
- `nix_value * nix_get_attr_byidx(nix_c_context * context, nix_value * value, EvalState * state, unsigned int i, const char ** name)` — attr by index + force.
- `nix_value * nix_get_attr_byidx_lazy(nix_c_context * context, nix_value * value, EvalState * state, unsigned int i, const char ** name)` — attr by index without force.
- `const char * nix_get_attr_name_byidx(nix_c_context * context, nix_value * value, EvalState * state, unsigned int i)` — attr name by index.
- `nix_err nix_init_bool(nix_c_context * context, nix_value * value, bool b)` — initializer.
- `nix_err nix_init_string(nix_c_context * context, nix_value * value, const char * str)` — initializer.
- `nix_err nix_init_path_string(nix_c_context * context, EvalState * s, nix_value * value, const char * str)` — initializer.
- `nix_err nix_init_float(nix_c_context * context, nix_value * value, double d)` — initializer.
- `nix_err nix_init_int(nix_c_context * context, nix_value * value, int64_t i)` — initializer.
- `nix_err nix_init_null(nix_c_context * context, nix_value * value)` — initializer.
- `nix_err nix_init_apply(nix_c_context * context, nix_value * value, nix_value * fn, nix_value * arg)` — initializer (thunk).
- `nix_err nix_init_external(nix_c_context * context, nix_value * value, ExternalValue * val)` — initializer.
- `nix_err nix_make_list(nix_c_context * context, ListBuilder * list_builder, nix_value * value)` — finalize a list.
- `ListBuilder * nix_make_list_builder(nix_c_context * context, EvalState * state, size_t capacity)` — start a list.
- `nix_err nix_list_builder_insert(nix_c_context * context, ListBuilder * list_builder, unsigned int index, nix_value * value)` — set index.
- `void nix_list_builder_free(ListBuilder * list_builder)` — free a list builder.
- `nix_err nix_make_attrs(nix_c_context * context, nix_value * value, BindingsBuilder * b)` — finalize an attrset.
- `nix_err nix_init_primop(nix_c_context * context, nix_value * value, PrimOp * op)` — initializer.
- `nix_err nix_copy_value(nix_c_context * context, nix_value * value, const nix_value * source)` — copy.
- `BindingsBuilder * nix_make_bindings_builder(nix_c_context * context, EvalState * state, size_t capacity)` — start an attrset.
- `nix_err nix_bindings_builder_insert(nix_c_context * context, BindingsBuilder * builder, const char * name, nix_value * value)` — insert attr.
- `void nix_bindings_builder_free(BindingsBuilder * builder)` — free a bindings builder.
- `nix_realised_string * nix_string_realise(nix_c_context * context, EvalState * state, nix_value * value, bool isIFD)` — realise a string with context.
- `const char * nix_realised_string_get_buffer_start(nix_realised_string * realised_string)` — pointer to data.
- `size_t nix_realised_string_get_buffer_size(nix_realised_string * realised_string)` — length.
- `size_t nix_realised_string_get_store_path_count(nix_realised_string * realised_string)` — paths count.
- `const StorePath * nix_realised_string_get_store_path(nix_realised_string * realised_string, size_t index)` — path by index.
- `void nix_realised_string_free(nix_realised_string * realised_string)` — free.

### Internal helpers / globals
- None.

### Type aliases
- `typedef enum { ... } ValueType` — value-type discriminator.
- `typedef struct nix_value nix_value` — opaque handle.
- `typedef struct EvalState EvalState` — opaque handle.
- `[[deprecated("use nix_value instead")]] typedef nix_value Value` — back-compat alias (uses C++ attribute syntax directly, not the `NIX_DEPRECATED` macro from `nix_api_expr.h`).
- `typedef struct BindingsBuilder BindingsBuilder` — opaque handle.
- `typedef struct ListBuilder ListBuilder` — opaque handle.
- `typedef struct PrimOp PrimOp` — opaque handle.
- `typedef struct ExternalValue ExternalValue` — opaque handle.
- `typedef struct nix_realised_string nix_realised_string` — opaque handle.
- `typedef void (*PrimOpFun)(void * user_data, nix_c_context * context, EvalState * state, nix_value ** args, nix_value * ret)` — primop callback signature.

### Macros
- None.

---

## File: src/libfetchers-c/nix_api_fetchers.cc

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block.

### Structs / enums (and what C++ types they wrap)
- None defined here.

### Exported functions (nix_*)
- `nix_fetchers_settings * nix_fetchers_settings_new(nix_c_context * context)` — allocate a `nix_fetchers_settings` whose `settings` field holds a `nix::ref<nix::fetchers::Settings>` made via `nix::make_ref<nix::fetchers::Settings>`. Note: this implementation does NOT clear the context error on entry (no `if (context) context->last_err_code = NIX_OK;` line).
- `void nix_fetchers_settings_free(nix_fetchers_settings * settings)` — `delete`.

### Internal helpers / globals
- None.

### Type aliases
- None.

### Macros
- None.

---

## File: src/libfetchers-c/nix_api_fetchers.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block (`// cffi start` only — no matching `// cffi end`).

### Structs / enums (and what C++ types they wrap)
- `struct nix_fetchers_settings` — opaque forward decl (definition in `nix_api_fetchers_internal.hh`).

### Exported functions (nix_*)
- `nix_fetchers_settings * nix_fetchers_settings_new(nix_c_context * context)` — allocate a settings handle.
- `void nix_fetchers_settings_free(nix_fetchers_settings * settings)` — free a settings handle.

### Internal helpers / globals
- None.

### Type aliases
- `typedef struct nix_fetchers_settings nix_fetchers_settings` — opaque handle.

### Macros
- None.

---

## File: src/libfetchers-c/nix_api_fetchers_internal.hh

### Namespaces / extern "C" blocks
- None (this is a `.hh` file with `#pragma once`, plain C++ linkage; no `extern "C"` wrapper).

### Structs / enums (and what C++ types they wrap)
- `struct nix_fetchers_settings { nix::ref<nix::fetchers::Settings> settings; }` — wraps a refcounted `fetchers::Settings`.

### Exported functions (nix_*)
- None.

### Internal helpers / globals
- None.

### Type aliases
- None.

### Macros
- None.

---

## File: src/libflake-c/nix_api_flake.cc

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block. All entry points use `nix_clear_err(context)` (rather than the inline `if (context) context->last_err_code = NIX_OK;` pattern).

### Structs / enums (and what C++ types they wrap)
- None defined here.

### Exported functions (nix_*)
- `nix_flake_settings * nix_flake_settings_new(nix_c_context * context)` — allocate `nix_flake_settings` whose `settings` field is `nix::make_ref<nix::flake::Settings>()`.
- `void nix_flake_settings_free(nix_flake_settings * settings)` — `delete`.
- `nix_err nix_flake_settings_add_to_eval_state_builder(nix_c_context * context, nix_flake_settings * settings, nix_eval_state_builder * builder)` — call `settings->settings->configureEvalSettings(builder->settings)` (adds `builtins.getFlake` etc.; the header warns this does not enable pure mode).
- `nix_flake_reference_parse_flags * nix_flake_reference_parse_flags_new(nix_c_context * context, nix_flake_settings * settings)` — allocate flags with `baseDirectory = std::nullopt`.
- `void nix_flake_reference_parse_flags_free(nix_flake_reference_parse_flags * flags)` — `delete`.
- `nix_err nix_flake_reference_parse_flags_set_base_directory(nix_c_context * context, nix_flake_reference_parse_flags * flags, const char * baseDirectory, size_t baseDirectoryLen)` — set `flags->baseDirectory` from a `std::string(baseDirectory, baseDirectoryLen)`.
- `nix_err nix_flake_reference_and_fragment_from_string(nix_c_context * context, nix_fetchers_settings * fetchSettings, nix_flake_settings * flakeSettings, nix_flake_reference_parse_flags * parseFlags, const char * strData, size_t strSize, nix_flake_reference ** flakeReferenceOut, nix_get_string_callback fragmentCallback, void * fragmentCallbackUserData)` — null-out `*flakeReferenceOut`, call `nix::parseFlakeRefWithFragment(*fetchSettings->settings, ..., parseFlags->baseDirectory, true)`, allocate the reference (via `nix::make_ref<nix::FlakeRef>`), stream the fragment via `call_nix_get_string_callback`.
- `void nix_flake_reference_free(nix_flake_reference * flakeReference)` — `delete`.
- `nix_flake_lock_flags * nix_flake_lock_flags_new(nix_c_context * context, nix_flake_settings * settings)` — allocate a `nix::ref<nix::flake::LockFlags>` initialized to `{recreateLockFile=false, updateLockFile=true, writeLockFile=true, failOnUnlocked=false, useRegistries=false, allowUnlocked=false, commitLockFile=false}` (the documented "write as needed" defaults).
- `void nix_flake_lock_flags_free(nix_flake_lock_flags * flags)` — `delete`.
- `nix_err nix_flake_lock_flags_set_mode_virtual(nix_c_context * context, nix_flake_lock_flags * flags)` — set `updateLockFile=true, writeLockFile=false, failOnUnlocked=false, allowUnlocked=true` (in-memory updates only).
- `nix_err nix_flake_lock_flags_set_mode_write_as_needed(nix_c_context * context, nix_flake_lock_flags * flags)` — set `updateLockFile=true, writeLockFile=true, failOnUnlocked=false, allowUnlocked=true` (on-disk updates if needed).
- `nix_err nix_flake_lock_flags_set_mode_check(nix_c_context * context, nix_flake_lock_flags * flags)` — set `updateLockFile=false, writeLockFile=false, failOnUnlocked=true, allowUnlocked=false` (fail if needs update).
- `nix_err nix_flake_lock_flags_add_input_override(nix_c_context * context, nix_flake_lock_flags * flags, const char * inputPath, nix_flake_reference * flakeRef)` — `NonEmptyInputAttrPath::parse` (throws `UsageError` on empty), insert into `inputOverrides`, then if `writeLockFile` was set, switch to virtual mode by calling `nix_flake_lock_flags_set_mode_virtual`.
- `nix_locked_flake * nix_flake_lock(nix_c_context * context, nix_fetchers_settings * fetchSettings, nix_flake_settings * flakeSettings, EvalState * eval_state, nix_flake_lock_flags * flags, nix_flake_reference * flakeReference)` — call `eval_state->state.resetFileCache()`, then `nix::flake::lockFlake(...)`, then wrap the result in `nix::make_ref<nix::flake::LockedFlake>`. Note: parameter `fetchSettings` is unused inside the body.
- `void nix_locked_flake_free(nix_locked_flake * lockedFlake)` — `delete`.
- `nix_value * nix_locked_flake_get_output_attrs(nix_c_context * context, nix_flake_settings * settings, EvalState * evalState, nix_locked_flake * lockedFlake)` — allocate a new value via `nix_alloc_value`, then call `nix::flake::callFlake(eval_state, *lockedFlake, *v->value)`. Note: parameter `settings` is unused inside the body.

### Internal helpers / globals
- None.

### Type aliases
- None.

### Macros
- None.

---

## File: src/libflake-c/nix_api_flake.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block (`// cffi start` only — no matching `// cffi end`).

### Structs / enums (and what C++ types they wrap)
- `struct nix_flake_settings` — opaque forward decl (wraps `nix::ref<nix::flake::Settings>`).
- `struct nix_flake_reference_parse_flags` — opaque forward decl (holds `optional<filesystem::path>`).
- `struct nix_flake_reference` — opaque forward decl (wraps `nix::ref<nix::FlakeRef>`).
- `struct nix_flake_lock_flags` — opaque forward decl (wraps `nix::ref<nix::flake::LockFlags>`).
- `struct nix_locked_flake` — opaque forward decl (wraps `nix::ref<nix::flake::LockedFlake>`).

### Exported functions (nix_*)
- `nix_flake_settings * nix_flake_settings_new(nix_c_context * context)` — new settings.
- `void nix_flake_settings_free(nix_flake_settings * settings)` — free.
- `nix_err nix_flake_settings_add_to_eval_state_builder(nix_c_context * context, nix_flake_settings * settings, nix_eval_state_builder * builder)` — configure `builtins.getFlake`.
- `nix_flake_reference_parse_flags * nix_flake_reference_parse_flags_new(nix_c_context * context, nix_flake_settings * settings)` — new parse flags.
- `void nix_flake_reference_parse_flags_free(nix_flake_reference_parse_flags * flags)` — free.
- `nix_err nix_flake_reference_parse_flags_set_base_directory(nix_c_context * context, nix_flake_reference_parse_flags * flags, const char * baseDirectory, size_t baseDirectoryLen)` — set base dir for relative refs.
- `nix_flake_lock_flags * nix_flake_lock_flags_new(nix_c_context * context, nix_flake_settings * settings)` — new lock flags.
- `void nix_flake_lock_flags_free(nix_flake_lock_flags * settings)` — free (parameter named `settings`).
- `nix_err nix_flake_lock_flags_set_mode_check(nix_c_context * context, nix_flake_lock_flags * flags)` — set "check" mode.
- `nix_err nix_flake_lock_flags_set_mode_virtual(nix_c_context * context, nix_flake_lock_flags * flags)` — set "virtual update" mode.
- `nix_err nix_flake_lock_flags_set_mode_write_as_needed(nix_c_context * context, nix_flake_lock_flags * flags)` — set "write as needed" mode.
- `nix_err nix_flake_lock_flags_add_input_override(nix_c_context * context, nix_flake_lock_flags * flags, const char * inputPath, nix_flake_reference * flakeRef)` — override an input.
- `nix_locked_flake * nix_flake_lock(nix_c_context * context, nix_fetchers_settings * fetchSettings, nix_flake_settings * settings, EvalState * eval_state, nix_flake_lock_flags * flags, nix_flake_reference * flake)` — lock the flake.
- `void nix_locked_flake_free(nix_locked_flake * locked_flake)` — free.
- `nix_err nix_flake_reference_and_fragment_from_string(nix_c_context * context, nix_fetchers_settings * fetchSettings, nix_flake_settings * flakeSettings, nix_flake_reference_parse_flags * parseFlags, const char * str, size_t strLen, nix_flake_reference ** flakeReferenceOut, nix_get_string_callback fragmentCallback, void * fragmentCallbackUserData)` — parse URL string into reference + fragment.
- `void nix_flake_reference_free(nix_flake_reference * store)` — free reference (parameter named `store`).
- `nix_value * nix_locked_flake_get_output_attrs(nix_c_context * context, nix_flake_settings * settings, EvalState * evalState, nix_locked_flake * lockedFlake)` — call the flake to get its attrset.

### Internal helpers / globals
- None.

### Type aliases
- `typedef struct nix_flake_settings nix_flake_settings`.
- `typedef struct nix_flake_reference_parse_flags nix_flake_reference_parse_flags`.
- `typedef struct nix_flake_reference nix_flake_reference`.
- `typedef struct nix_flake_lock_flags nix_flake_lock_flags`.
- `typedef struct nix_locked_flake nix_locked_flake`.

### Macros
- None.

---

## File: src/libflake-c/nix_api_flake_internal.hh

### Namespaces / extern "C" blocks
- None (`.hh` file with `#pragma once`, plain C++ linkage; no `extern "C"` wrapper).

### Structs / enums (and what C++ types they wrap)
- `struct nix_flake_settings { nix::ref<nix::flake::Settings> settings; }`.
- `struct nix_flake_reference_parse_flags { std::optional<std::filesystem::path> baseDirectory; }`.
- `struct nix_flake_reference { nix::ref<nix::FlakeRef> flakeRef; }`.
- `struct nix_flake_lock_flags { nix::ref<nix::flake::LockFlags> lockFlags; }`.
- `struct nix_locked_flake { nix::ref<nix::flake::LockedFlake> lockedFlake; }`.

### Exported functions (nix_*)
- None.

### Internal helpers / globals
- None.

### Type aliases
- None.

### Macros
- None.

---

## File: src/libmain-c/nix_api_main.cc

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block.

### Structs / enums (and what C++ types they wrap)
- None.

### Exported functions (nix_*)
- `nix_err nix_init_plugins(nix_c_context * context)` — call `nix::initPlugins()`.
- `nix_err nix_set_log_format(nix_c_context * context, const char * format)` — call `nix::setLogFormat`; rejects NULL with `NIX_ERR_UNKNOWN` ("Log format is null") before entering the try block.

### Internal helpers / globals
- None.

### Type aliases
- None.

### Macros
- None.

---

## File: src/libmain-c/nix_api_main.h

### Namespaces / extern "C" blocks
- One `extern "C" { ... }` block (`// cffi start`/`// cffi end`).

### Structs / enums (and what C++ types they wrap)
- None.

### Exported functions (nix_*)
- `nix_err nix_init_plugins(nix_c_context * context)` — load plugins per `plugin-files`.
- `nix_err nix_set_log_format(nix_c_context * context, const char * format)` — choose a log formatter by name.

### Internal helpers / globals
- None.

### Type aliases
- None.

### Macros
- None.

---

## File: src/clang-tidy-plugin/nix-clang-tidy-checks.cc

### Namespaces / extern "C" blocks
- `namespace nix::clang_tidy { ... }`. No `extern "C"`.

### Structs / enums (and what C++ types they wrap)
- `class NixClangTidyChecks : public clang::tidy::ClangTidyModule` — empty plugin module skeleton; overrides `addCheckFactories(ClangTidyCheckFactories &)` (parameter marked `[[maybe_unused]]`) with an empty body and an inline comment showing the recipe for registering future checks. The file-level comment documents the broader recipe (header + source + register call + meson + .clang-tidy enable).

### Exported functions (nix_*)
- None.

### Internal helpers / globals
- `static clang::tidy::ClangTidyModuleRegistry::Add<NixClangTidyChecks> X("nix-module", "Adds Nix-specific checks")` — registry entry that registers the module under the name `nix-module`.

### Type aliases
- `using namespace clang;` and `using namespace clang::tidy;` brought in inside the `nix::clang_tidy` namespace.

### Macros
- None.

---

## File: src/nswrapper/nswrapper.cc

### Namespaces / extern "C" blocks
- `namespace nix { ... }`. No `extern "C"` (this is a standalone Linux-only executable).

### Structs / enums (and what C++ types they wrap)
- None.

### Exported functions (nix_*)
- None.

### Internal helpers / globals
- `static uid_t parseUid(std::string_view value)` — parse a uid/gid from a string via `nix::string2Int<uid_t>`, throwing `UsageError("Not a valid integer")` on failure.
- `void mainWrapped(int argc, char ** argv)` — the body of the helper. Validates `argc >= 4`, parses `first_uid num_uids command [args...]`, sanity-checks that `numExtra > 0`, that the extra-uid range doesn't wrap, and that the range doesn't include the current uid or gid. Forks a child via `startProcess` that (after waiting for the parent to unshare) calls `newuidmap` and `newgidmap` via `runProgram`; meanwhile the parent enters new user+mount namespaces (`unshare(CLONE_NEWUSER | CLONE_NEWNS)`), signals the child via a `Pipe`, waits for it, then `setresuid(0,0,0)` + `setresgid(0,0,0)`, drops supplementary groups via `setgroups(0, nullptr)`, mounts a fresh `devpts` on `/dev/pts` with `mode=0620`, and `execvp`'s the requested command with `argv+3`.

### Exported (top-level) entry point
- `int main(int argc, char ** argv)` — `nix::handleExceptions(argv[0], [&]() { nix::mainWrapped(argc, argv); })`.

### Type aliases
- None.

### Macros
- None.

---

## Cross-file observations

### Init-function family
Each library exposes a parallel `nix_<libname>_init` that is idempotent and forwards to a corresponding C++ `nix::init<Lib>()`:

- `nix_libutil_init` -> `nix::initLibUtil`.
- `nix_libstore_init` and `nix_libstore_init_no_load_config` -> `nix::initLibStore[(false)]`.
- `nix_libexpr_init` is the one outlier — it composes `nix_libutil_init`, `nix_libstore_init`, and `nix::initGC()`. Callers therefore do not need to explicitly init lower layers before initing libexpr (though they are still allowed to, since each init is idempotent).
- `nix_init_plugins` (libmain) -> `nix::initPlugins`. Note this is not under a `nix_libmain_init`-style name; it is the only init in libmain.
- libfetchers-c and libflake-c have no separate init function — they piggyback on the general `nix_libexpr_init` chain.

### Error-reporting family
Every fallible exported function exhibits the same skeleton:
1. `if (context) context->last_err_code = NIX_OK;` (libutil-c, libstore-c, libexpr-c, libmain-c, libfetchers-c) or the equivalent `nix_clear_err(context)` (libflake-c).
2. `try { ... } NIXC_CATCH_ERRS{,_RES,_NULL}`.

`nix_fetchers_settings_new` is a minor outlier: it does not clear the context on entry (the `if (context) ...` line is missing), though the catch macro still records errors there.

The three macros live in `src/libutil-c/nix_api_util_internal.h`. There is consistent reuse of `nix_set_err_msg` for non-exceptional failure paths (e.g. `NIX_ERR_KEY` for OOB list/attr indices in `nix_api_value.cc`, NULL-pointer guards in `nix_store_copy_path`, `NIX_ERR_UNKNOWN` for the "Log format is null" guard in `nix_set_log_format`).

### Opaque-pointer accessor pattern
Several wrapper structs follow a one-field "opaque -> C++" pattern, all of which could be expressed
by a single helper template were the ABI not constrained to plain C-style structs:

- `Store { nix::ref<nix::Store> ptr; }`
- `StorePath { nix::StorePath path; }`
- `nix_derivation { nix::Derivation drv; }`
- `BindingsBuilder { nix::BindingsBuilder builder; }`
- `ListBuilder { nix::ListBuilder builder; }`
- `nix_string_return { std::string str; }`
- `nix_printer { std::ostream & s; }` (reference, not value)
- `nix_string_context { nix::NixStringContext & ctx; }` (reference, not value)
- `nix_fetchers_settings { nix::ref<nix::fetchers::Settings> settings; }`
- `nix_flake_settings { nix::ref<nix::flake::Settings> settings; }`
- `nix_flake_reference { nix::ref<nix::FlakeRef> flakeRef; }`
- `nix_flake_lock_flags { nix::ref<nix::flake::LockFlags> lockFlags; }`
- `nix_locked_flake { nix::ref<nix::flake::LockedFlake> lockedFlake; }`
- `nix_flake_reference_parse_flags { std::optional<std::filesystem::path> baseDirectory; }`

Two are subtly different: `EvalState` and `nix_value`. `EvalState` keeps a reference plus three
optionally-owning smart pointers (`unique_ptr<fetchers::Settings>`, `unique_ptr<EvalSettings>`,
`shared_ptr<EvalState>`) because the C handle is sometimes used as a bare wrapper around a borrowed
`nix::EvalState &` (in primop callbacks via `EvalState wrapper{state};` and similarly in the
JSON/XML paths of `NixCExternalValue`) and sometimes as the sole owner constructed via
`nix_eval_state_build` (placement-new + explicit dtor in `nix_state_free`).
`nix_value` carries an extra `nix::EvalMemory *` because the stable C ABI cannot retroactively pass
an `EvalMemory` argument through `nix_init_string` etc.; the comment in
`nix_api_expr_internal.h` is explicit that this is a stable-ABI workaround.

`nix_realised_string { std::string str; std::vector<StorePath> storePaths; }` is the one wrapper
with two fields (the realised string content plus the C-API `StorePath`s referenced by it).

`nix_c_context` has four fields and is the only wrapper that is conceptually owned by the C side
and passed back into Nix entry points rather than vice versa.

### Lifecycle naming pairs
The shard uses two related naming conventions for lifecycle:

- `*_new`/`*_free` (libfetchers-c, libflake-c): the newer style.
- `*_open`/`*_alloc_*`/`*_create_*` paired with `*_free` (libstore-c, libexpr-c values, libexpr-c primops): the older style.

A second axis is "free" vs "decref": opaque handles that are pure RAII C++ wrappers (`Store`,
`StorePath`, `nix_derivation`, all flake structs, `nix_fetchers_settings`,
`nix_eval_state_builder`, `EvalState`, `nix_realised_string`) are released via `nix_*_free` (plain
`delete`, possibly preceded by an explicit destructor call for `EvalState`), while
GC-managed objects (`nix_value`, `PrimOp`, `ExternalValue`) use `nix_gc_decref` /
`nix_value_decref`. The header documentation explicitly notes the migration intent away from the
generic `nix_gc_decref` toward typed wrappers.

The two builders (`BindingsBuilder`, `ListBuilder`) are GC-aware in a different way: they are
allocated via placement-new with `(NoGC)` under Boehm and freed via `GC_FREE` (or plain `delete`
without GC), but they do not participate in the `nix_refcounts` table.

### Forced vs lazy variants
`nix_api_value.{cc,h}` provides parallel "forced" and `_lazy` variants for list and attr access:
`nix_get_list_byidx{,_lazy}`, `nix_get_attr_byname{,_lazy}`, `nix_get_attr_byidx{,_lazy}`. The
lazy variants are nearly-identical copies of the non-lazy ones with the `forceValue` call removed
and slightly different error messages on OOB; for `nix_get_list_byidx` the lazy variant also drops
the post-lookup `if (p == nullptr) return nullptr;` guard. The duplication is the most obvious
internal symmetry in the entire shard.

### Layered-attrs collapse
`nix_get_attr_byidx`, `nix_get_attr_byidx_lazy`, and `nix_get_attr_name_byidx` all call the same
static helper `collapse_attrset_layer_chain_if_needed` to flatten layered attrsets before doing
index-based access. That helper itself is private to `nix_api_value.cc` and uses
`state.buildBindings` + `mkAttrs` to materialize a flat copy.

### Builder ergonomics
`BindingsBuilder` and `ListBuilder` follow identical lifecycles (allocated via `*_make_*_builder`
under `(NoGC)` placement, mutated with `*_insert`, finalized with `*_make_{list,attrs}` which moves
the contents into the value, and then released with `*_free` that prefers `GC_FREE` over `delete`
when Boehm GC is enabled). The two are independent code paths but the structure is so symmetric
that consolidation is plausible.

### Refcounting bookkeeping
`nix_gc_incref`/`nix_gc_decref` are no-ops without Boehm GC. With Boehm, they share a single
`boost::concurrent_flat_map` named `nix_refcounts`, keyed by `const void *`, with a
`traceable_allocator<std::pair<const void * const, unsigned int>>` so the map itself is GC-aware.
`nix_value_incref`/`_decref` are pure forwarders, kept as the typed public API the headers say is
preferred over the generic versions. `nix_create_external_value` and `nix_alloc_primop` both call
`nix_gc_incref(nullptr, ret)` to seed their refcount.

### Header style
Every public header pairs a Doxygen `@defgroup` block (`libutil`, `libstore`, `libstore_storepath`,
`libstore_derivation`, `libexpr`, `libfetchers`, `libflake`, `libmain`) with `// cffi start`
markers. The `// cffi end` marker is present in libutil, libstore, libstore subheaders, libexpr,
libexpr-external, libexpr-value, and libmain headers; it is absent from
`src/libfetchers-c/nix_api_fetchers.h` and `src/libflake-c/nix_api_flake.h` — minor but visible
inconsistency.

The two `*_internal.h` files (libutil, libstore) wrap their contents in `extern "C"`; the two
`*_internal.hh` files (libfetchers, libflake) do not (they rely on plain C++ linkage and are only
included from `.cc` files in the same directory). The libexpr family is a single
`*_internal.h` with `extern "C"`.

### Outliers
- `src/clang-tidy-plugin/nix-clang-tidy-checks.cc` is a stub: no checks are registered, only the
  module-registry entry. The only non-trivial content is the file-level comment that documents the
  recipe for adding a check.
- `src/nswrapper/nswrapper.cc` is a standalone Linux helper, not part of the C ABI. It has no
  `nix_*` exports; it exists to call `newuidmap`/`newgidmap` from outside the new user namespace
  (because the helpers cannot be invoked on a process already in its target namespace) and then
  `unshare` + `mount` + `execvp` the desired command in a fresh namespace. Its only intersection
  with this shard is that it links against libutil (`SysError`, `UsageError`, `Pid`, `Pipe`,
  `runProgram`, `readLine`, `writeFull`, `toOsStrings`, `string2Int`, `handleExceptions`,
  `startProcess`, `statusOk`).
