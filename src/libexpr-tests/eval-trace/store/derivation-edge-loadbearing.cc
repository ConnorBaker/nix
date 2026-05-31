/**
 * DECISION-CRITICAL adversarial probe: is the flattened FileBytes dep LOAD-BEARING
 * for derivation-consumer soundness, or does StorePathAvailability(.drv) alone
 * catch input changes?
 *
 * derivation-input-flattening.cc proved that observing a derivation's outPath
 * records BOTH (a) the flattened input FileBytes AND (b) a StorePathAvailability
 * edge on the .drv. The content-addressed-trace-identity RFC proposes REPLACING
 * the flattened inputs with a single edge. For that to be sound, the edge must
 * itself track input identity.
 *
 * StorePathAvailability verifies via isValidPath(storePathStr) — a pure existence
 * check keyed on the (OLD) .drv path string (store-path-availability.cc). So the
 * question that decides the RFC's edge representation:
 *
 *   After the derivation's input file changes, is the OLD .drv path STILL VALID
 *   in the store?
 *
 *   - If YES: StorePathAvailability(oldDrv) would still pass → if it were the
 *     consumer's ONLY dep, the consumer would STALE-SERVE the old outPath. The
 *     flattened FileBytes is therefore LOAD-BEARING, and the RFC's edge must be a
 *     producer-TRACE-HASH edge (folds in inputs), NOT the existing SPA dep.
 *   - If NO (old drv evicted/never written): SPA alone catches the change; the
 *     edge could reuse SPA.
 *
 * This probe settles it on the real evaluator before any hot-path change.
 */
#include "eval-trace/helpers.hh"

#include <format>

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// Record the deps of `d.outPath` for a derivation whose args embed a file's
// content. Extract the recorded .drv store path. Mutate the file. Then ask the
// store: is the OLD .drv path still valid? The answer decides whether SPA is
// sufficient on its own.
TEST_F(MaterializationDepTest, DrvOutPath_OldDrvValidityAfterInputChange)
{
    TempTextFile shared("payload-v1");
    auto expr = std::format(
        R"(let
             content = builtins.readFile {0};
             d = derivation {{
               name = "p";
               builder = "/bin/sh";
               system = builtins.currentSystem;
               args = [ content ];
             }};
           in d.outPath)",
        shared.path.string());

    // Cold record; capture the recorded .drv path from the root trace's deps.
    std::string oldDrvPath;
    {
        auto cache = makeCache(expr);
        forceRoot(*cache);
    }
    {
        auto rootDeps = getStoredDeps("");  // root attr path
        GTEST_LOG_(INFO) << "root deps (v1):\n" << dumpDeps(rootDeps);
        for (const auto & d : rootDeps) {
            if (d.type == CanonicalQueryKind::StorePathAvailability) {
                oldDrvPath = d.key;
                break;
            }
        }
    }
    ASSERT_FALSE(oldDrvPath.empty())
        << "expected a StorePathAvailability dep on the .drv";
    GTEST_LOG_(INFO) << "old .drv path = " << oldDrvPath;

    // Is the old .drv valid in the store right now (before mutation)?
    bool validBefore = false;
    try {
        validBefore = state.store->isValidPath(state.store->parseStorePath(oldDrvPath));
    } catch (...) {}
    GTEST_LOG_(INFO) << "old .drv valid BEFORE mutation = " << validBefore;

    // Mutate the derivation's input file. drvPath = hashDerivationModulo(args) so
    // the NEW eval would compute a DIFFERENT drvPath. But does the OLD one remain
    // valid?
    shared.modify("payload-v2");
    invalidateFileCache(shared.path);

    bool validAfter = false;
    try {
        validAfter = state.store->isValidPath(state.store->parseStorePath(oldDrvPath));
    } catch (...) {}
    GTEST_LOG_(INFO) << "old .drv valid AFTER mutation = " << validAfter;

    // The finding: if the old drv stays valid, StorePathAvailability(oldDrv) would
    // PASS on the warm verify of the OLD trace — meaning, were SPA the consumer's
    // only dep, it would serve the stale old outPath. We log the verdict; the
    // load-bearing conclusion drives the RFC's edge representation.
    GTEST_LOG_(INFO) << "FILEBYTES-LOAD-BEARING VERDICT: oldDrvStillValid="
                     << validAfter
                     << " (true ⇒ SPA-alone would stale-serve ⇒ flattened "
                        "FileBytes is load-bearing ⇒ RFC edge must be a "
                        "producer-trace-hash, not the existing SPA dep)";

    // Weakest enforced fact: the .drv path was recorded and parseable. The
    // validity verdict is the characterization payload (logged above).
    EXPECT_FALSE(oldDrvPath.empty());
}

} // namespace nix::eval_trace
