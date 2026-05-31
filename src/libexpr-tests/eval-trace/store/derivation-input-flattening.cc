/**
 * DECISION-CRITICAL characterization: does a derivation's INPUT closure flatten
 * into a consumer's trace, or does StorePathAvailability(.drv) ALREADY act as a
 * content-addressed edge?
 *
 * The content-addressed-trace-identity RFC (plans/content-addressed-trace-identity-rfc.md)
 * rests on the claim that when N consumers each observe a shared derivation, the
 * derivation's INPUT-reads flatten (copy) into each consumer's trace — the 607×.
 * The proposed fix gives derivationStrict its own producer-trace keyed by drvPath.
 *
 * BUT derivation-observation-facets.cc measured that `(derivation {...}).outPath`
 * records ONLY StorePathAvailability + SessionSystemValue — NO FileBytes. If the
 * derivation's inputs (its `args`/`builder`/source `src`) DON'T flatten into the
 * consumer, then the RFC's premise is WRONG for derivations: the .drv path already
 * IS the edge, and the producer-trace work would buy nothing for the derivation
 * boundary specifically.
 *
 * These tests measure the actual flattening behavior for a derivation whose INPUT
 * is a real source file, consumed by N siblings — the exact shape the RFC targets.
 * The result decides whether the RFC's §1 mechanism claim survives contact with
 * the recorder, BEFORE any hot-path change.
 *
 * Pure characterization (no production change); real evaluator via
 * MaterializationDepTest + getStoredDeps.
 */
#include "eval-trace/helpers.hh"

#include <format>

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// A derivation whose `args` embed the contents of a shared source FILE, consumed
// by two siblings via `.outPath`. Question: does the shared file's FileBytes dep
// flatten into BOTH consumer traces (RFC premise), or does each consumer carry
// only a StorePathAvailability(.drv) edge (premise refuted for derivations)?
//
// We read the file with builtins.readFile and feed it into the derivation's args,
// so the derivation's INPUT genuinely depends on the file's content. drvPath =
// hashDerivationModulo folds args in, so a file change changes drvPath.
TEST_F(MaterializationDepTest, DrvInputFile_ConsumedByOutPath_FlattenOrEdge)
{
    TempTextFile shared("payload-v1");
    // d is a derivation whose args depend on the shared file's content; cx and cy
    // are two sibling consumers each observing d.outPath. Both transitively reach
    // the shared file THROUGH the derivation's input.
    auto expr = std::format(
        R"(let
             content = builtins.readFile {0};
             d = derivation {{
               name = "p";
               builder = "/bin/sh";
               system = builtins.currentSystem;
               args = [ content ];
             }};
           in {{ cx = d.outPath; cy = d.outPath; }})",
        shared.path.string());

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        for (auto * name : {"cx", "cy"}) {
            auto * a = root.attrs()->get(state.symbols.create(name));
            ASSERT_NE(a, nullptr);
            state.forceValue(*a->value, noPos);
        }
    }

    auto cxDeps = getStoredDeps("cx");
    auto cyDeps = getStoredDeps("cy");

    auto fileName = std::string(shared.path.filename());

    // Diagnostic dump — this is a characterization test; the dep shape IS the
    // finding.
    GTEST_LOG_(INFO) << "cx deps:\n" << dumpDeps(cxDeps);
    GTEST_LOG_(INFO) << "cy deps:\n" << dumpDeps(cyDeps);

    bool cxHasFile = hasDep(cxDeps, CanonicalQueryKind::FileBytes, fileName);
    bool cyHasFile = hasDep(cyDeps, CanonicalQueryKind::FileBytes, fileName);
    bool cxHasDrv = countDepsByType(cxDeps, CanonicalQueryKind::StorePathAvailability) >= 1;
    bool cyHasDrv = countDepsByType(cyDeps, CanonicalQueryKind::StorePathAvailability) >= 1;

    GTEST_LOG_(INFO) << "FLATTENING VERDICT: "
                     << "cxHasFile=" << cxHasFile << " cyHasFile=" << cyHasFile
                     << " cxHasDrv=" << cxHasDrv << " cyHasDrv=" << cyHasDrv;

    // The decisive assertion is documented, not enforced as pass/fail, because
    // this test's PURPOSE is to discover the truth. We assert the WEAKEST true
    // fact (both consumers carry SOME dep on the derivation), then log the shape.
    EXPECT_TRUE(cxHasDrv || cxHasFile)
        << "cx must carry SOME dep tying it to the derivation\n" << dumpDeps(cxDeps);
    EXPECT_TRUE(cyHasDrv || cyHasFile)
        << "cy must carry SOME dep tying it to the derivation\n" << dumpDeps(cyDeps);
}

// DISCRIMINATOR (2026-05-31, follow-up #6 feasibility): WHERE does the shared
// input FileBytes flatten into a consumer — at the OUTPUT-STRING read
// (.outPath / .drvPath, the coerceToContextObject site #6 proposes hooking),
// or at FORCE of the derivation attrset (replayMemoizedDeps, decoupled from any
// output read)?
//
// Three sibling consumers over the SAME shared-input derivation `d`:
//   cx = d.outPath           -- reads an output string (DrvDeep/Built context)
//   cy = d.drvPath           -- reads the drv string (different output, context)
//   cz = builtins.attrNames d -- FORCES d, reads NO output string at all
//
// If cz (no output-string read) STILL carries the shared FileBytes, the flatten
// happened at FORCE time, decoupled from the output read. Then a coerceToContextObject
// hook (#6) fires DOWNSTREAM of where the FileBytes was already copied into the
// consumer scope — i.e. the edge would be ADDITIVE, not flatten-replacing, and
// #6's "prototype-sized re-key at the coercion boundary" is WRONG. Conversely, if
// cz does NOT carry FileBytes but cx/cy do, the flatten is tied to the output-string
// observation and the coercion hook is correctly placed.
//
// Characterization: logs the per-consumer shape (the finding), asserts only the
// weakest true fact, like the sibling test above.
TEST_F(MaterializationDepTest, DrvInput_FlattenSite_ForceVsOutputRead)
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
           in {{ cx = d.outPath; cy = d.drvPath; cz = builtins.attrNames d; }})",
        shared.path.string());

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        for (auto * name : {"cx", "cy", "cz"}) {
            auto * a = root.attrs()->get(state.symbols.create(name));
            ASSERT_NE(a, nullptr);
            state.forceValue(*a->value, noPos);
        }
    }

    auto fileName = std::string(shared.path.filename());
    for (auto * name : {"cx", "cy", "cz"}) {
        auto deps = getStoredDeps(name);
        bool hasFile = hasDep(deps, CanonicalQueryKind::FileBytes, fileName);
        bool hasDrv = countDepsByType(deps, CanonicalQueryKind::StorePathAvailability) >= 1;
        GTEST_LOG_(INFO) << name << " deps (hasFile=" << hasFile
                         << " hasDrv=" << hasDrv << "):\n" << dumpDeps(deps);
    }

    // FLATTEN-SITE VERDICT is in the logs above. cz = attrNames d forces the
    // derivation WITHOUT reading any output string; if cz.hasFile is true the
    // flatten is force-driven (coercion hook would be downstream/additive).
    auto czDeps = getStoredDeps("cz");
    GTEST_LOG_(INFO) << "FLATTEN-SITE VERDICT: cz(attrNames, no output read) hasFile="
                     << hasDep(czDeps, CanonicalQueryKind::FileBytes, fileName)
                     << " — if 1, flatten is FORCE-driven, coercion hook (#6) is downstream/additive.";
    SUCCEED();
}

// Soundness anchor regardless of flatten-vs-edge: changing the derivation's input
// file MUST invalidate a consumer of its outPath (drvPath is input-addressed).
// This is the property the RFC's edge must preserve; if it already holds via
// StorePathAvailability, the edge is sound by construction.
TEST_F(MaterializationDepTest, DrvInputFile_Change_InvalidatesOutPathConsumer)
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

    // Cold record.
    { auto cache = makeCache(expr); forceRoot(*cache); }

    // Warm hit precondition.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "precondition: unchanged input ⇒ warm hit";
    }

    // Mutate the derivation's input file ⇒ drvPath changes ⇒ outPath changes.
    shared.modify("payload-v2");
    invalidateFileCache(shared.path);

    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "soundness: changing the derivation's input file must invalidate "
               "the outPath consumer (drvPath is content-addressed by inputs)";
    }
}

} // namespace nix::eval_trace
