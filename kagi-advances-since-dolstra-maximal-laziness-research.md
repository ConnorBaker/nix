## Advances Since Dolstra's "Maximal Laziness" (2008)

Yes, there have been significant advances, particularly in **efficient hashing modulo alpha-equivalence** and **content-addressed code**. The Unison work is indeed related, though it addresses a different (simpler) problem than what Nix faces.

---

### 1. Hashing Modulo Alpha-Equivalence (PLDI 2021)

Maziarz, Ellis, Lawrence, Fitzgibbon, and Peyton Jones published **"Hashing Modulo Alpha-Equivalence"** at PLDI 2021, achieving **O(n log²n)** complexity for identifying alpha-equivalent subterms. [^2][^4]

**Key insight**: Use a **weak (commutative) hash combiner** (XOR) for the free-variable map, which allows efficient incremental updates when variables are bound:

```haskell
-- Hash of variable map = XOR of entry hashes
hashVM :: VarMap -> HashCode
hashVM m = foldr xor 0 [entryHash k v | (k,v) <- toList m]

-- Removing an entry: just XOR again (since a ⊕ a = 0)
removeEntry :: Name -> VarMap -> VarMap
```

The algorithm separates the problem into two steps: [^4]
1. **E-summary**: A compositional representation that captures structure + free-variable positions (no information loss)
2. **Hashing**: Convert e-summaries to fixed-size hash codes (information-losing but with provably low collision probability)

**Collision bound**: For expressions e₁, e₂ that are *not* α-equivalent: [^6]
$$P(\text{hash}(e_1) = \text{hash}(e_2)) \leq \frac{5(|e_1| + |e_2|)}{2^b}$$

---

### 2. Context-Sensitive Alpha-Equivalence (PLDI 2024)

Blaauwbroek, Olšák, and Geuvers improved this to **O(n log n)** with **"Hashing Modulo Context-Sensitive α-Equivalence"**. [^5]

**The problem with standard α-equivalence**: When comparing subterms within a larger context, standard α-equivalence either equates too few or too many terms depending on how you handle free variables. [^3]

**Context-sensitive α-equivalence** compares open terms *within a context* that resolves their free variables. This coincides with **bisimulation equivalence** when viewing λ-terms as graphs. [^2]

**Trade-offs between the two approaches**: [^3]
- Standard α-equivalence is simpler but can't be properly defined on open de Bruijn terms
- Context-sensitive α-equivalence handles any representation and equals graph bisimilarity
- For CSE in compilers, both work since you typically want the *largest* equivalent subterms

---

### 3. Unison's Content-Addressed Code

Unison uses **512-bit SHA3 hashes** of syntax trees, with names excluded from the hash. [^1][^8]

**How Unison handles alpha-equivalence**: Named arguments are replaced by **positionally-numbered variable references** (similar to de Bruijn indices): [^1]

```
-- Source
increment x = Nat.+ x 1

-- What gets hashed (conceptually)
increment = (#arg1 -> #a8s6df921a8 #arg1 1)
```

Dependencies are replaced by their hashes, so `increment`'s hash uniquely identifies its implementation *and* pins all dependencies. [^1]

**Relationship to Nix**:

| Aspect | Unison | Nix |
|--------|--------|-----|
| When hashing occurs | At definition time (static) | Would need to be at runtime (dynamic) |
| What's hashed | Syntax trees of definitions | Closures (expr + env) or values |
| Laziness | Strict evaluation | Lazy - can't force to hash |
| Type system | Static types guide equivalence | Dynamic - must inspect values |

**Key difference**: Unison hashes *code* (which is static and immutable), while Nix needs to hash *runtime closures* (which may contain unevaluated thunks). Unison's approach is directly applicable to **AST interning** in Nix, but not to runtime memoization.

---

### 4. HVM/Bend and Interaction Nets (2022-2024)

A significant recent development is **HVM** (Higher-order Virtual Machine) and the **Bend** language, which use **interaction nets** for optimal parallel λ-reduction. [^1][^2]

**Key properties**:
- **Optimal sharing**: Never duplicates work (Lévy-optimal reduction)
- **Automatic parallelism**: No explicit thread management
- **No GC needed**: Memory management via interaction net structure

**Relevance to Nix**: Interaction nets represent a fundamentally different approach to sharing—instead of memoizing results, they ensure computations are *never duplicated* by construction. This is theoretically interesting but would require a complete rewrite of the Nix evaluator.

---

### 5. GHC's Recent Sharing Work (2024)

Tweag's work on **observable type sharing** in GHC Core is relevant: 

**Key insight**: Instead of sharing via meta-level indirections (pointers in memory), use **object-level indirections** (let-bound variables in the IR). This makes sharing:
- **Observable**: Part of the syntax, not hidden in memory
- **Serializable**: Survives across compilation phases
- **Optimizable**: Can apply CSE and let-floating to types

This is analogous to what Nix would need: making sharing explicit in the representation rather than relying on pointer identity.

---

### 6. Self-Adjusting Computation

Acar's work on **self-adjusting computation** (2005-present) provides a framework for incremental memoization: 

**Core idea**: Programs automatically adjust to input changes by tracking dependencies and re-executing only affected computations.

**Key techniques**:
- **Memoization with change propagation**: Cache results keyed by input identity
- **Traceable data types**: Track dependencies at the ADT level, not implementation level
- **Non-monotonic memoization**: Handle cases where cache entries become invalid

This is relevant to Nix's goal of avoiding redundant evaluation across multiple Nixpkgs instances.

---

### 7. Hash-Consing with Weak References

Recent work on hash-consing in symbolic computation (2025) confirms the standard approach: [^7][^8]

```
A global hash table is central to the hash consing mechanism. 
To balance memory efficiency with lookup performance, we employed 
weak references for storing symbolic objects. This design allows 
the garbage collector (GC) to reclaim memory for terms that are 
no longer strongly referenced elsewhere in the program.
```

This aligns with the approach outlined in the previous research synthesis for Nix.

---

### Synthesis: What's Applicable to Nix

| Technique | Applicability | Notes |
|-----------|--------------|-------|
| **Alpha-equivalence hashing** (PLDI 2021/2024) | **High** for AST interning | Directly applicable to `Expr` canonicalization |
| **Unison's content-addressing** | **Medium** | Good model for AST hashing, but doesn't solve runtime memoization |
| **HVM/Interaction nets** | **Low** (short-term) | Would require fundamental redesign |
| **GHC observable sharing** | **High** | Model for making sharing explicit in IR |
| **Self-adjusting computation** | **Medium** | Framework for incremental evaluation |
| **Weak-reference hash-consing** | **High** | Standard technique, works with Boehm GC |

**The fundamental gap remains**: All these advances address hashing of *static* structures (syntax trees, types). Nix's challenge is hashing *runtime closures* containing unevaluated thunks without forcing them.

**The solution path** (as outlined in the previous synthesis):
1. **Hash-cons the AST** using alpha-equivalence hashing techniques
2. **Canonicalize environments** so same bindings → same `Env*`
3. **Intern thunks** by `(Expr*, Env*)` pairs
4. **Memoize by identity** (pointer pairs), not by structural hash of values

This sidesteps the "hash without forcing" problem by ensuring that **pointer equality implies semantic equality** through canonicalization, rather than trying to compute structural hashes of lazy values.

[^1]: [Lazy Evaluation Research Papers - Academia.edu](https://www.academia.edu/Documents/in/Lazy_Evaluation) (28%)
[^2]: [CSE 413, Spring 2023, Lazy Evaluation Summary](https://courses.cs.washington.edu/courses/cse413/23sp/lectures/lazysum.pdf) (21%)
[^3]: [Lazy evaluation - Wikipedia](https://en.wikipedia.org/wiki/Lazy_evaluation) (16%)
[^4]: [Memoization - Wikipedia](https://en.wikipedia.org/wiki/Memoization) (13%)
[^5]: [Verifying Resource Bounds of Programs with Lazy Evaluation and ...](https://www.researchgate.net/publication/295121587_Verifying_Resource_Bounds_of_Programs_with_Lazy_Evaluation_and_Memoization) (8%)
[^6]: [Preserving Sharing in the Partial Evaluation of Lazy Functional ...](https://www.researchgate.net/publication/221495788_Preserving_Sharing_in_the_Partial_Evaluation_of_Lazy_Functional_Programs) (5%)
[^7]: [Unison: A Content-Addressable Programming Language](https://news.ycombinator.com/item?id=22156370) (5%)
