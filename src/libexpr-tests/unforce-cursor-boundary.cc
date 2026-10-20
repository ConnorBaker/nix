/* An eval-cache `AttrCursor` that roots a `Value *` inside an unforce scope
   must re-derive that value once the scope has ended, never read the stale
   (possibly freed) cell. This is exactly what the `cacheGeneration` /
   `isStale` staleness stamps in `src/libexpr/eval-cache.cc` protect: a
   `Value *` cached while `isolationDepth > 0` records `unforceGeneration()`,
   and once a scope has ended (bumping the generation and freeing the job
   region) `getValue()` must notice the stamp is stale and derive the value
   again from its parent.

   The earlier functional gate (`nix flake show`) never reached this: no
   `AttrCursor` was read AFTER the generation advanced past its stamp (the
   generation bumps only at the outermost scope end, after the whole
   traversal), so `isStale` was true 0 times and forcing it to `return false`
   changed nothing. This white-box test forces the exact ordering and makes
   the re-derivation observable.

   A stateful root loader returns a different value the second time it runs,
   so the two outcomes are distinguishable (the same probe technique the
   `resetFileCache` identity test uses to observe a revert-and-rebuild):

     - if the held cursor RE-DERIVES after the boundary (the guard fires), it
       re-runs the loader and sees the second value;
     - if it returns its pre-boundary snapshot (guard defeated), it sees the
       first value, read out of a cell the job end already freed.

   The cursor roots a depth-2 attribute (`x.s`) whose cell is YOUNG (allocated
   in the job region), and a second job reuses and overwrites that region, so
   the defeated-guard read is a genuine use-after-free, not merely the wrong
   logical value. The test asserts the re-derived (second) value. With the
   guard defeated (`isStale` forced to `return false`, or the generation bump
   skipped) it reads the first value / freed cell and fails. */

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/unforce.hh"

#include <gtest/gtest.h>

namespace nix {

namespace {

/* Run `f` as one job (outermost unforce scope) with batch size 0. */
template<typename F>
void job(EvalState & state, F && f)
{
    UnforceBatch batch(state);
    {
        UnforceBatch::Element element(batch);
        f();
    }
    batch.end();
}

} // namespace

class UnforceCursorBoundaryTest : public LibExprTest
{
protected:
    UnforceCursorBoundaryTest()
    {
        evalSettings.unforce = true;
        evalSettings.unforceBatchSize = 0;
    }

    /* A cell allocated outside any job, so older than every job, left as a
       thunk: forcing it happens inside a job below, which puts its young
       allocations (including the inner `s` cell) in the job region. */
    Value * oldThunk(std::string_view expr)
    {
        auto v = state.allocValue();
        state.eval(state.parseExprFromString(std::string(expr), state.rootPath(CanonPath::root)), *v);
        return v;
    }
};

/* Skip when unforcing is compiled out (Boehm GC): the scope never frees, so
   there is nothing to go stale. */
#if !NIX_USE_BOEHMGC

TEST_F(UnforceCursorBoundaryTest, cursorHeldAcrossBatchBoundaryReForces)
{
    using namespace eval_cache;

    /* Two distinct root values. `x.s` is 777777 the first time the loader
       runs and 999999 the second; so the value the cursor reports after the
       boundary says which path it took. `pad` makes the forced attrset large
       enough to land in the region. */
    auto root1 = oldThunk("{ x = { s = 777777; pad = builtins.genList (i: i * 3) 256; }; }");
    auto root2 = oldThunk("{ x = { s = 999999; pad = builtins.genList (i: i * 5) 256; }; }");
    int loads = 0;
    auto cache = make_ref<EvalCache>(std::nullopt, state, [&]() -> Value * {
        return loads++ == 0 ? root1 : root2;
    });

    std::shared_ptr<AttrCursor> sCur;
    NixInt::Inner firstRead = 0;
    job(state, [&] {
        auto root = cache->getRoot();
        auto xCur = root->getAttr("x");
        sCur = xCur->getAttr("s");
        /* Forces `s` and stamps `_valueGeneration` with this job's generation
           (isolationDepth > 0). The cursor now roots a cell in this job's
           region. */
        firstRead = sCur->getInt().value;
    });
    ASSERT_EQ(firstRead, 777777);
    ASSERT_EQ(loads, 1);

    /* The job has ended: the generation advanced and the region that held the
       `s` cell was freed. Reuse that region with a second job so a stale read
       would see overwritten memory, not coincidentally-intact bytes. */
    job(state, [&] {
        auto filler = state.allocValue();
        state.eval(
            state.parseExprFromString(
                "builtins.genList (i: { a = i; b = i * 7; c = i * 9; }) 20000", state.rootPath(CanonPath::root)),
            *filler);
        state.forceValueDeep(*filler);
    });

    /* Re-read the held cursor across the boundary. `isStale` fires along the
       whole cursor chain (every stamp predates the generation bump), so the
       cursor re-derives: it re-runs the loader (-> root2) and reports 999999.
       With the guard defeated it returns the freed snapshot (777777 or
       garbage) and never re-runs the loader. */
    auto got = sCur->getInt();
    EXPECT_EQ(got.value, 999999);
    EXPECT_EQ(loads, 2) << "the root loader was not re-run: the held cursor did not re-derive after the batch boundary";
}

#endif

} // namespace nix
