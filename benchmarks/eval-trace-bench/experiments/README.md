# eval-trace ad-hoc experiments

One-off measurement scripts that answer a specific go/no-go question against an
already-built `result/bin/nix`, without a full `eval-trace-bench generate` run.
Each script documents its question, method, and recorded result in its header;
the narrative analysis lives in the design docs it cross-references.

These are intentionally lightweight (no flake app, no run-number bookkeeping) —
they exploit `NIX_SHOW_STATS` counters and per-run isolated `XDG_CACHE_HOME`
rather than the full harness. Prefer the `eval-trace-bench` flake app for
wall-time A/Bs; use these for counter-level mechanism questions.

| Script | Question | Result |
|---|---|---|
| `ca-producer-sibling-firerate.sh` | Does the §3b CA-producer replay gate's fire-rate (E) catch up to its producer-record cost (P) on a sibling-share-heavy `python3Packages` workload? | **FALSIFIED** — E:P ≈ 0.03 flat, worse than the 0.096 `closures.gnome` baseline; the gate is keyed to the `strict` attrset siblings never re-force (`derivation.nix` `//`-wrapper). See `doc/eval-trace-cache-redesign-plan.md` follow-up #5. |
| `layer2a-crash-consistency.sh` | Does Layer 2a (deferring `publishStateChange`) close the #24 Sessions-before-Traces ordering inversion that `deferFlush` alone opens? Mid-eval SIGKILL + reopen + dangling-`trace_id` query; the Layer-1-alone arm (rebuilt via `REBUILD_L1_ALONE=1`) proves non-vacuity. | **CONFIRMED** — Layer-1-alone reproduces the hazard (175..760 dangling Sessions / 194..1024 History across 6 kills); Layer 2a leaves 0/0. SIGKILL-timed, so not a CI test. See `doc/eval-trace-cache-redesign-plan.md` follow-up #24 and `plans/async-producer-recording-plan.md` §8. |
