---
synopsis: "The evaluator batches the store writes it makes during evaluation"
prs: []
---

The evaluator now buffers the store derivations and `builtins.toFile` files it creates during evaluation and writes them to the store in one batch when one of their paths first leaves the evaluator, or when evaluation ends, instead of writing each one as soon as it is created.  It is always on and has no eager alternative; the store's contents after a command are unchanged.

The buffer bounds its own memory: it flushes once it holds more than `deferred-store-writes-max-pending` objects (default 4096), so peak memory does not grow with the size of the evaluated set, even for `nix eval --json` and `--raw`, which build their whole output in memory before writing.

For consumers of the C++ API of `libexpr`: a string value's bytes are no longer readable through `Value::string_view()`, `c_str()` or `string_data()`, which are private.  Text leaves the evaluator through `EvalState::realise`, `realiseNoCtx`, `emit` and `coerceAndEmit`, which write the pending store objects the text may name before handing it out; inside the evaluator the accessors `forceString`, `forceStringNoCtx` and `coerceToString` are unchanged.  `printValueAsJSON` and `printValueAsXML` no longer take a stream: `renderValueAsJSON` and `renderValueAsXML` return the document as a string.  The exec helpers of the `nix` command take an `EvalState::finish()` token.  For the C API: every function that receives an evaluator writes what evaluation left pending before it returns, on success and on failure alike, and a `nix_value` handle is invalid once its evaluator has been freed.
