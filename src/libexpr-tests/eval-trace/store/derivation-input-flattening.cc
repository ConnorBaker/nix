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

// DISCRIMINATOR (2026-05-31, follow-up #6/#7/#8): WHERE does the shared input
// FileBytes flatten into a consumer — at the OUTPUT-STRING read (.outPath /
// .drvPath, the coerceToContextObject site #6 proposed hooking), or at FORCE of
// the derivation's ARGS (derivationStrict's forceAttrs, decoupled from any output
// read)?
//
// HISTORY: follow-up #7's first cut used only cz = attrNames d and concluded
// "flatten is output-read-driven" from cz.hasFile=0. That was a CONFOUND — cz
// varies TWO axes vs cx/cy (no output read AND attrNames doesn't force the args).
// #8's adversarial pass split the axes and REFUTED #7:
//   cx = d.outPath                                  -- forces args, reads output
//   cy = d.drvPath                                  -- forces args, reads drvPath
//   cz = builtins.attrNames d                       -- keys only, does NOT force args
//   cw = deepSeq (removeAttrs d [outputs]) null     -- forces args DEEPLY, NO output read
//   cv = seq d null                                 -- WHNF only, does NOT force args
//
// MEASURED: cx=1 cy=1 cw=1 ; cz=0 cv=0. The decisive case is cw: it forces the
// derivation's args deeply but reads NO output string, and STILL carries FileBytes.
// => the flatten is FORCE-OF-ARGS-driven, NOT output-read-driven. cz=0 was because
// attrNames doesn't force the args (same as cv), not because it skipped an output
// read. CONSEQUENCE: a coerceToContextObject hook at the output read fires AFTER
// derivationStrict already recorded FileBytes into the consumer's scope — it would
// be ADDITIVE, not flatten-replacing. #6's hook-site claim is REFUTED. See
// redesign-plan follow-up #8.
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
           in {{
                cx = d.outPath;                                  # output read (string + Built ctx)
                cy = d.drvPath;                                  # output read (drvPath string + DrvDeep ctx)
                cz = builtins.attrNames d;                       # keys only — may not force args
                cw = builtins.deepSeq (removeAttrs d ["outPath" "drvPath" "out"]) null; # forces d's NON-output attrs deeply (incl args/builder), NO output-string read
                cv = builtins.seq d null;                        # forces d's spine only (WHNF)
              }})",
        shared.path.string());

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        for (auto * name : {"cx", "cy", "cz", "cw", "cv"}) {
            auto * a = root.attrs()->get(state.symbols.create(name));
            ASSERT_NE(a, nullptr);
            state.forceValue(*a->value, noPos);
        }
    }

    auto fileName = std::string(shared.path.filename());
    for (auto * name : {"cx", "cy", "cz", "cw", "cv"}) {
        auto deps = getStoredDeps(name);
        bool hasFile = hasDep(deps, CanonicalQueryKind::FileBytes, fileName);
        bool hasDrv = countDepsByType(deps, CanonicalQueryKind::StorePathAvailability) >= 1;
        GTEST_LOG_(INFO) << name << " deps (hasFile=" << hasFile
                         << " hasDrv=" << hasDrv << "):\n" << dumpDeps(deps);
    }

    // CONFOUND-RESOLVING VERDICT. The first version of this test had ONE axis
    // for cz (attrNames) but it varied TWO things vs cx/cy: (a) no output-string
    // read AND (b) attrNames may not force the derivation's args at all. cz=0
    // could therefore mean either. cw and cv split the axes:
    //   cw = deepSeq (removeAttrs d [outputs]) — forces d's args/builder DEEPLY,
    //        reads NO output string. If cw.hasFile=1, the input flatten is
    //        FORCE-driven (reading the output string is NOT what carries it),
    //        and the #6 coerceToContextObject hook would be DOWNSTREAM/ADDITIVE.
    //   cv = seq d — WHNF only (does not force args). Control for "attrset spine".
    // The honest reading of the matrix is logged; the proposal's hook-site claim
    // stands ONLY if cx/cy carry FileBytes while cw does NOT.
    bool cwHasFile = hasDep(getStoredDeps("cw"), CanonicalQueryKind::FileBytes, fileName);
    bool cxHasFile = hasDep(getStoredDeps("cx"), CanonicalQueryKind::FileBytes, fileName);
    GTEST_LOG_(INFO) << "FLATTEN-SITE VERDICT (confound-split): "
                     << "cx(output read) hasFile=" << cxHasFile << " ; "
                     << "cw(forces args deeply, NO output read) hasFile=" << cwHasFile
                     << " — if cw=1, flatten is FORCE-driven and the #6 coercion hook is "
                        "DOWNSTREAM/ADDITIVE (proposal hook-site REFUTED). "
                        "If cw=0 and cx=1, the output-string read is what carries it (hook-site OK).";
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

// RFC obligation pin (derivation-producer-partition-rfc.md §2/§7.1): a derivation
// forces input-reads that do NOT fold into its drvPath. With __ignoreNulls=true
// (nixpkgs mkDerivation DEFAULT), derivationStrict forces each attr to test for
// null and DROPS it via `continue` if null (primops.cc:1795-1798) — so a
// file-backed attr that evaluates to null records a readFile dep but does not
// affect drvPath. Two different file contents that both keep the attr null yield
// the IDENTICAL drvPath, yet the consumer MUST invalidate when that file changes.
//
// This test pins the CONSERVATIVE-shape soundness for that case (it must hold
// today) AND is the red obligation any future producer-ISOLATION prototype must
// keep green: if isolation keys the producer by drvPath alone, this read is lost
// and the consumer would stale-serve. Verified end-to-end by adversarial pass #2
// (Attack E): the conservative shape re-records on the cond change.
TEST_F(MaterializationDepTest, DrvIgnoreNullsDroppedRead_ChangeInvalidatesConsumer)
{
    TempTextFile cond("aaa");
    // optionalAttr is null unless cond=="yes\n"; with __ignoreNulls it is dropped
    // from the derivation, so cond's content does NOT change drvPath while staying
    // non-"yes". But derivationStrict forces optionalAttr (to test null), recording
    // a readFile dep on cond. Consumer reads drvPath.
    auto expr = std::format(
        R"(let
             condStr = builtins.readFile {0};
             optionalAttr = if condStr == "yes\n" then "present" else null;
             d = derivation {{
               name = "nulltest";
               builder = "/bin/sh";
               system = builtins.currentSystem;
               __ignoreNulls = true;
               extra = optionalAttr;
               args = [ "x" ];
             }};
           in d.drvPath)",
        cond.path.string());

    // Cold record (cond="aaa").
    { auto cache = makeCache(expr); forceRoot(*cache); }

    // Warm-hit precondition: cond unchanged ⇒ hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "precondition: unchanged cond ⇒ warm hit";
    }

    // Mutate cond "aaa" -> "bbb": optionalAttr stays null (dropped), so drvPath is
    // UNCHANGED — but the recorded readFile(cond) dep changed. The consumer MUST
    // re-evaluate. A producer keyed by drvPath alone would NOT (that is the RFC's
    // core soundness obligation).
    cond.modify("bbb");
    invalidateFileCache(cond.path);

    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "RFC §2 obligation: a __ignoreNulls-dropped file-backed attr's read "
               "must invalidate the consumer even though drvPath is unchanged "
               "(drvPath does not encode dropped-attr reads)";
    }
}

} // namespace nix::eval_trace
