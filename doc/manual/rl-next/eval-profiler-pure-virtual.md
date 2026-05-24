---
synopsis: "`EvalProfiler`'s hook methods are now pure virtual"
---

`EvalProfiler::preFunctionCallHook`, `EvalProfiler::postFunctionCallHook`, and `EvalProfiler::getNeededHooks` are now pure virtual; subclasses must implement all three. The previous default no-op bodies and the lazy `getNeededHooksImpl` non-virtual-interface cache are gone — the per-`EvalState` snapshot of which hooks each profiler wants is now captured once at construction and never re-queried, so the cache earned nothing.

External `EvalProfiler` subclasses (in plugins, Hydra, Lix, or third-party embedders) that previously relied on the no-op defaults must now implement at least empty bodies for any hook they don't override. This is a source-compat break only on the public `EvalProfiler` interface in `nix/expr/eval-profiler.hh`; in-tree subclasses (`FunctionCallTrace`, `SampleStack`) already overrode all three.
