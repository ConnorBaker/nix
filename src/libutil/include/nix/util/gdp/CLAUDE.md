# GDP (Ghosts of Departed Proofs) Library

C++ approximation of the GDP pattern from Haskell (Noonan, Haskell
Symposium 2018). A `Proof<Tag>` is a zero-size, non-constructible token
that exists only as `const &` inside a scoped continuation, proving that a
precondition (identified by `Tag`) holds. It is not a globally sealed
capability: C++ cannot prevent new `Certifier<Tag>` subclasses.

## Headers

| Header | Purpose |
|--------|---------|
| `proof.hh` | `Proof<Tag>`, `Certifier<Tag>` (parameterized private inheritance) |
| `proof-guarded.hh` | `ProofGuarded<T, Tag>` — data accessible only with matching proof |

## API

### `Proof<Tag>`
Zero-size proof token. Cannot be constructed, copied, or moved by user
code. Created only inside continuations via `Certifier<Tag>::withProof`
or `Certifier<Tag>::withProofIf`. No bare proof objects exist — proofs
live only inside the continuation passed to these methods.

### `Certifier<Tag>`
Base for custom certifiers. Parameterized private inheritance (not CRTP):
subclass privately as `class MyClass : private Certifier<MyTag>` and call
the two protected static methods from domain-specific code. Private
inheritance restricts proof creation to methods of the inheriting class,
but does not prevent other code from defining a different inheriting class.

**Protected static methods:**

- **`withProof(f)`** — Unconditionally creates a scoped `Proof<Tag>` and
  passes it to `f` as `const Proof<Tag> &`. Returns whatever `f` returns.

- **`withProofIf(cond, f)`** — Conditionally creates a scoped `Proof<Tag>`.
  Calls `f` only if `cond` is true. Non-void `f`: returns
  `std::optional<R>` (nullopt when `!cond`). Void `f`: returns `bool`
  (false when `!cond`).

### `ProofGuarded<T, Tag>`
Data wrapper that requires `const Proof<Tag> &` for access. Generalizes
the strand-local pattern. Non-copyable, non-movable — proof-guarded data
is bound to a specific proof context (e.g., a strand) and should not be
duplicated.

## Eval-trace instances

| Eval-trace type | GDP mechanism |
|----------------|--------------|
| `StrandToken<Tag>` | `gdp::Proof<Tag>` (alias) |
| `StrandLocal<T, Tag>` | `gdp::ProofGuarded<T, Tag>` (alias) |
| `BlockingScope` | `gdp::Proof<BlockingTag>` via `Certifier<BlockingTag>` on `TraceStore` and `BlockingThreadPool` |
| `BlockingTag` proof | `Certifier<BlockingTag>` on `TraceStore` (lifecycle) and `BlockingThreadPool` (async) |
| `DedupCheckedTag` proof | `Certifier<DedupCheckedTag>::withProofIf` on `DedupGate` |
| `RecordingScopeActiveTag` proof | `Certifier<RecordingScopeActiveTag>` on `RecordingScopeGuard` |
| `DepCaptureScopeTag` proof | `Certifier<DepCaptureScopeTag>` on `DepCaptureScope` and `SiblingForceScope` |
| Strand tokens | `Certifier<FileStrandTag>` on `FileStrandGate`, `Certifier<VerificationAccessTag>` on `Verifier` |
| Test blocking proofs | `Certifier<BlockingTag>` inheritance + `withProof` (scoped in each test method) |
| File subsumption | Not GDP: eval-trace uses an opaque capability type (`VerifiedFileDep`) because public proof tags are subclass-forgeable in C++ |

## C++ caveats

- **Temporary materialization**: `withProof` creates the `Proof<Tag>`
  as a temporary via `Proof<Tag>{Key{}}` and binds it to `const &`.
  This works because C++17 guarantees that the temporary is materialized
  directly in the function's local storage and the `const &` extends its
  lifetime. The deleted copy/move constructors are never invoked.

- **Friend matching**: `Proof` friends only `Certifier<Tag>` (simple
  class template friend). The previous eval-trace pattern friended
  free function templates with complex signatures, which is fragile.

- **`optional<void>` is ill-formed**: `withProofIf` uses `if constexpr`
  to handle void vs non-void return types in a single function body.

- **Inherent C++ gap**: Anyone can subclass `Certifier<Tag>` — C++ has
  no sealed classes. Private inheritance limits who can *call* the
  protected methods to each inheriting class, but it cannot prevent new
  subclasses elsewhere in the program. This is the remaining gap compared
  to Haskell GDP, where rank-2 polymorphism makes proof escape
  structurally impossible. For high-blast-radius operations, prefer an
  opaque capability object with a private constructor over a public proof
  tag API.

## Rule Of Thumb

- Use GDP when the proof only needs to exist inside the immediate scoped
  continuation and no long-lived fact escapes from it.
- Do not use GDP alone when the operation mutates cached verification
  state, subsumption state, or any other state whose effects outlive the
  proving scope.
- In those cases, manufacture an opaque capability with a private
  constructor and keep the friend/factory set minimal.

## Comparison with Haskell GDP

| Haskell GDP | This library |
|-------------|-------------|
| Phantom type parameter `ph` | `Tag` template parameter |
| `forall ph. (Proof ph -> r) -> r` | `Certifier<Tag>::withProof(f)` |
| `SuchThat ph p` | `ProofGuarded<T, Tag>` |
| GHC enforces rank-2 scoping | C++ approximation via non-copyable `const &` |

The Haskell version uses rank-2 polymorphism to make proof escape
*impossible* (the phantom `ph` is universally quantified). The C++ version
uses non-copyable, non-movable `const &` — escape is prevented at
construction time, not by the type system itself.

## Maintenance

When modifying the GDP library, update this file and:

**This CLAUDE.md must be updated when:**
- Changing `Proof<Tag>` or `Certifier<Tag>` API (withProof/withProofIf signatures)
- Changing `ProofGuarded<T, Tag>` access semantics
- Adding new eval-trace proof tags → update "Eval-trace instances" table

**Cross-reference with:**
- `src/libexpr/eval-trace/CLAUDE.md` Section 5 (GDP Continuations) — lists all
  proof tags and their certifiers
- `doc/eval-trace/implementation.md` Section 3.1 (GDP Proof Enforcement table)
- `doc/eval-trace/design.md` Section 2.5 (GDP description) and Section 3.3
  (type-safety patterns table)

**Verification after changes:**
```bash
# All GDP certifier instantiations in eval-trace
grep -rP 'Certifier<\w+Tag>' src/libexpr/eval-trace/ src/libexpr/include/nix/expr/eval-trace/ --include='*.hh' --include='*.cc' | grep -oP 'Certifier<(\w+)>' | sort -u
```
