---
synopsis: "Cheaper materialisation of dirty Git working trees and locked inputs"
---

Evaluating or building from a dirty Git working tree no longer re-copies the
whole tree into the store on every edit. When a source crosses into a
derivation, Nix now materialises the committed base tree once and *assembles*
the dirty store path from it: files unchanged from the last commit are
reflinked (copy-on-write, on filesystems such as btrfs/xfs) or hardlinked from
the base, and only the changed files are written. Editing one file in a large
monorepo therefore allocates storage for that one file rather than for the
whole tree. The assembled path is named from the recomputed NAR hash of its
actual contents, so its content always matches its store-path name; if the
assembled result would not match the path the evaluator expects, Nix falls
back to a full copy.

Relatedly, a flake input that is already locked to a Git revision is no longer
walked an extra time on a cold store just to recompute the NAR hash that the
lock already records: its store path is named directly from the recorded hash
and the byte-copy is deferred until the source is actually built. A consumer
that only reads metadata (`outPath`, `rev`, `lastModified`) of such an input
does no tree walk at all.

These optimisations are sound by construction: a mutable `path:` input keeps
eager hash verification (the deferral applies only to inputs pinned to an
immutable Git revision, never to `path:`), and the dirty-tree assembler falls
back to a full copy whenever its result would not match the expected content.
