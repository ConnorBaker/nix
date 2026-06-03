#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "nix/store/derivations.hh"
#include "nix/store/store-open.hh"
#include "nix/store/tests/libstore.hh"
#include "nix/store/tests/derivation.hh"

/**
 * Projection laws (LD-P*) for `.drv` materialisation as a content-addressed
 * `Projection<>` (PROPOSAL-LAZY-DERIVATIONS.md §0.5 / §11, layer 1).
 *
 * These exercise the **eager key**: `computeStorePath(drv) =
 * makeFixedOutputPathFromCA(SHA256, unparse(drv))` — a pure function of the
 * in-memory `drv` that writes nothing. They are what makes deferral sound:
 * the path is known before (and independently of) the bytes hitting disk.
 */
namespace nix {

/* LD-P1 — key purity / eager identity: `computeStorePath` is deterministic
   and has no store effect (computing the key materialises nothing).
   Negative control: `isValidPath(key) == false` after a compute-only call. */
RC_GTEST_FIXTURE_PROP(LibStoreTest, LazyDrv_KeyIsPureNoStoreEffect, (const Derivation & drv))
{
    auto k1 = computeStorePath(*store, drv);
    auto k2 = computeStorePath(*store, drv);
    RC_ASSERT(k1 == k2);               // deterministic
    RC_ASSERT(!store->isValidPath(k1)); // computing the key wrote nothing
}

/* LD-P2 — mint == write: the path returned at deferred instantiation equals
   the path eventually registered. On the dummy store `writeDerivation`
   computes the same key and records validity; the in-tree backstop is the
   `assert(path2 == path)` in `Store::writeDerivation` (derivations.cc). */
RC_GTEST_FIXTURE_PROP(LibStoreTest, LazyDrv_ComputedPathEqualsWrittenPath, (const Derivation & drv))
{
    /* The default `dummy://` is read-only; a writable one records the drv
       in-memory without the reference-validity checks a real on-disk store
       would impose on arbitrary generated derivations. */
    auto wstore = openStore("dummy://?read-only=false");
    auto minted = computeStorePath(*wstore, drv);
    auto written = wstore->writeDerivation(drv);
    RC_ASSERT(minted == written);
    RC_ASSERT(wstore->isValidPath(written));
}

/* LD-P3 — soundness / content discrimination: any field mutation changes the
   key. Adding a fresh env entry must change the unparsed ATerm, hence the
   hash. (Negative-direction control of LD-P1's determinism.) */
RC_GTEST_FIXTURE_PROP(LibStoreTest, LazyDrv_MutatedFieldChangesKey, (const Derivation & drv, const std::string & k))
{
    /* `__json` is reserved by the ATerm encoding; an empty key and existing
       keys would not be a *fresh* mutation. */
    RC_PRE(!k.empty() && k != "__json" && !drv.env.contains(k));
    auto drv2 = drv;
    drv2.env.insert_or_assign(k, "x");
    RC_ASSERT(computeStorePath(*store, drv) != computeStorePath(*store, drv2));
}

/* LD-P3 (positive control) — structurally equal derivations hash to the SAME
   key (the converse of `MutatedFieldChangesKey`: content-addressing is a
   function, not merely injective). */
RC_GTEST_FIXTURE_PROP(LibStoreTest, LazyDrv_EqualDrvsSameKey, (const Derivation & drv))
{
    auto drv2 = drv; // structurally equal copy
    RC_ASSERT(computeStorePath(*store, drv) == computeStorePath(*store, drv2));
}

/* LD-P4 — cross-process determinism (R2): `computeStorePath` consumes no heap
   pointer, so two independent store instances (same store dir) agree on the
   key. This is why a second process cache-hits the same `drvPath`. */
RC_GTEST_FIXTURE_PROP(LibStoreTest, LazyDrv_KeyStableAcrossStates, (const Derivation & drv))
{
    auto store2 = openStore("dummy://");
    RC_ASSERT(computeStorePath(*store, drv) == computeStorePath(*store2, drv));
}

} // namespace nix
