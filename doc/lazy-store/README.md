# Lazy store: the design documents

These documents specify the lazy-store work of this branch and say how it is checked: the evaluator's deferred store writes (the derivations and `toFile` texts it creates are written in one batch when one of their paths first leaves the evaluator); the naming of every source by its git tree hash; the store's **one address**, the object hash — a store object's content hash is the git identifier of its root under SHA-256, the NAR a serialisation whose hash is computed when an old form asks for it; and the object store beneath the local store, which keeps every store object's trees and blobs as git objects and shares identical files unconditionally. They state meaning, laws, decisions and the checks that discharge them, and cite the code and the tests. They do not record history.

## The documents

| Document | What it holds | Read it when |
|---|---|---|
| `01-specification.md` | **The meaning, the laws and the decisions.** What a source and a store object mean (a content-addressed tree with one address, the object hash, and the NAR as its serialisation); the correctness conditions (C1), (C1'), (C2) and what they force on an implementation; the freedoms; the one intended difference from master; what is excluded; the four tree operations and the naming law; the naming of sources (9.9), the object store (9.10) and the one address (9.11), each with its laws and the checks they induce; every decision with its reason and consequence (10); the open items with a recommendation each (13) | First, and before reopening any decision |
| `04-derivation.md` | **The implementation, component by component.** The write buffer, the doors and dereferences through which text and paths leave the evaluator (the table the check script enforces), the accessors' naming, why sources are not deferred, the object store's composite and visitors, the one address's types and forms; the risks a maintainer relies on; the defects of master this work found | Before changing `src/libexpr`, the fetchers' naming, or an ingestion route |
| `05-validation.md` | **How the branch is checked.** The suites and the command line for each; the differential harness against master and how to read a run; the (C2) observer; the mutation checks that must fail; what each platform finds; the measurements with their commands; the tests owed; the rules for changing this code | Before declaring a change checked, and when adding a check |
| `07-defect-ledger.md` | **Defects pinned by a named test.** Each defect the branch had, and the test that fails on the code that had it | Before touching a component, to find the test that guards it |
| `08-store-model.md` | **The local store's model.** The locks and their scopes, temp roots, what may happen between a write and a registration, the durability points, the object store's invariants kept and not kept, the order the sinks fire — one fact per sentence with its `file:line`; the checklist of API semantics to answer at every `flock`/`*at`/`fsync`/libgit2/protocol call site | Before touching `src/libstore` or a sink |

`runs/` holds the Linux measurement's driver (`linux-measure-object-store.nix`, `linux-timing-object-store.sh`), which `05-validation.md` section 6 runs.

## Conventions

- Section numbers are stable because the code and the tests cite them (`git grep lazy-store/ -- src tests maintainers`); a gap in a document's numbering is a section removed.
- Locators are `path:line` in the tree; line numbers drift, and a locator names the function beside it so that it can be found again. Where a document describes master it says so, and its present tense is master's.
- Δ (or Δc) is the set of store objects, build-trace entries and permanent roots the reference adds while running a command.
- A number is marked in place with its source: the command that produced it, or its arithmetic and the mark `[derived]`; a number with neither is marked `[unverified]` or `[estimated]`. Timings taken on macOS are directions only; constants are Linux's (`05-validation.md` section 6).
- A test is named as its suite runs it (suite and name for a unit test, the script's path under `tests/functional` for a functional test); a test written in italics is owed and does not exist yet (`05-validation.md` section 7).
