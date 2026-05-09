/// store-path-coercion.cc — Tests for path → string coercion with copyToStore.
///
/// Verifies that files within a store tree (e.g., flake source root)
/// get independently content-addressed store paths when coerced, not
/// subpath references into the containing tree.  The string context
/// must reference the independent file, not the containing root.

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/eval.hh"
#include "nix/util/source-path.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/serialise.hh"
#include "nix/store/store-api.hh"

#include <gtest/gtest.h>

namespace nix {

/// Fixture with a write-enabled dummy store.
class StorePathCoercionTest : public LibExprTest
{
public:
    StorePathCoercionTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {
    }

protected:
    /// Add a directory tree to the store via NAR serialization.
    StorePath addTreeToStore(
        const std::string & name,
        const std::vector<std::pair<std::string, std::string>> & files)
    {
        auto mem = make_ref<MemorySourceAccessor>();
        for (auto & [fname, content] : files) {
            std::string c(content);
            mem->addFile(CanonPath("/" + fname), std::move(c));
        }

        StringSink sink;
        SourcePath(mem, CanonPath::root).dumpPath(sink);

        StringSource source(sink.s);
        return state.store->addToStoreFromDump(
            source, name,
            FileSerialisationMethod::NixArchive,
            ContentAddressMethod::Raw::NixArchive,
            HashAlgorithm::SHA256);
    }

    /// Coerce a path value to string with copyToStore=true.
    std::string coercePath(const SourcePath & path, NixStringContext & context)
    {
        Value v;
        v.mkPath(path, state.mem);
        auto result = state.coerceToString(noPos, v, context,
            "store-path-coercion-test", false, true);
        return std::string(std::move(result).toOwned());
    }
};

// ── Test 1a: File within store tree → independent store path ────────

TEST_F(StorePathCoercionTest, FileInStoreTree_GetsIndependentStorePath)
{
    auto treeStorePath = addTreeToStore("source", {
        {"builder.sh", "#!/bin/sh\necho hello"},
        {"other.sh", "#!/bin/sh\necho other"},
    });

    // Construct a path to builder.sh through storeFS (as flakes do).
    auto treePath = state.storePath(treeStorePath);
    auto builderPath = SourcePath(treePath.accessor, treePath.path / "builder.sh");

    NixStringContext context;
    auto result = coercePath(builderPath, context);

    // Must be /nix/store/<hash>-builder.sh, NOT /nix/store/<tree>-source/builder.sh
    auto resultStorePath = state.store->parseStorePath(result);
    EXPECT_NE(resultStorePath, treeStorePath)
        << "Must be an independent store path, not the containing tree";
    EXPECT_EQ(result, state.store->printStorePath(resultStorePath))
        << "Result must be exactly the store path string — no subpath suffix";
}

// ── Test 1b: Store tree root → keeps root store path ────────────────

TEST_F(StorePathCoercionTest, StoreTreeRoot_KeepsRootStorePath)
{
    auto treeStorePath = addTreeToStore("source", {
        {"flake.nix", "{}"},
    });

    NixStringContext context;
    auto result = coercePath(state.storePath(treeStorePath), context);

    auto resultStorePath = state.store->parseStorePath(result);
    EXPECT_EQ(resultStorePath, treeStorePath)
        << "Root coercion must return the tree's own store path";
}

// ── Test 2a: Context for file → references independent path ─────────

TEST_F(StorePathCoercionTest, FileInStoreTree_ContextReferencesFile)
{
    auto treeStorePath = addTreeToStore("source", {
        {"hook.sh", "#!/bin/sh\necho hook"},
        {"unrelated.txt", "data"},
    });

    auto treePath = state.storePath(treeStorePath);
    auto hookPath = SourcePath(treePath.accessor, treePath.path / "hook.sh");

    NixStringContext context;
    auto result = coercePath(hookPath, context);

    ASSERT_EQ(context.size(), 1u)
        << "Coerced file must have exactly one context entry";

    auto & elem = *context.begin();
    auto * opaque = std::get_if<NixStringContextElem::Opaque>(&elem.raw);
    ASSERT_NE(opaque, nullptr)
        << "Context entry must be Opaque";

    EXPECT_NE(opaque->path, treeStorePath)
        << "Context must NOT reference the containing tree root";

    EXPECT_EQ(state.store->printStorePath(opaque->path), result)
        << "Context must reference the same independent store path as the result";
}

// ── Test 2b: Context for root → references root ─────────────────────

TEST_F(StorePathCoercionTest, StoreTreeRoot_ContextReferencesRoot)
{
    auto treeStorePath = addTreeToStore("source", {
        {"flake.nix", "{}"},
    });

    NixStringContext context;
    auto result = coercePath(state.storePath(treeStorePath), context);

    ASSERT_EQ(context.size(), 1u);

    auto * opaque = std::get_if<NixStringContextElem::Opaque>(&context.begin()->raw);
    ASSERT_NE(opaque, nullptr);

    EXPECT_EQ(opaque->path, treeStorePath)
        << "Context for root coercion must reference the tree root itself";
}

// ── Test 3: Accumulated context preserved across coercion ────────────
//
// Verifies the context accumulator contract: pre-existing context
// entries are preserved across a copyToStore coercion, and the
// coercion adds its own entry without disturbing the accumulator.
//
// This is a structural test for the fix to the IFD regression where
// copyPathToStoreViaEvalEnvironment leaked the outer context accumulator
// into the copy request.  The full IFD regression requires a rootFS
// path inside a real Nix store (not reproducible with the dummy store),
// but this test verifies the accumulator contract that the fix enforces.

TEST_F(StorePathCoercionTest, AccumulatedContext_PreservedAcrossCoercion)
{
    auto treeStorePath = addTreeToStore("source", {
        {"patch.txt", "--- a/file\n+++ b/file\n"},
    });

    // Create a fake derivation for a Built context entry.
    Derivation drv;
    drv.name = "fake-bootstrap";
    drv.platform = "x86_64-linux";
    drv.builder = "/bin/sh";
    drv.args = {"-c", "echo hello > $out"};
    drv.outputs.insert_or_assign("out", DerivationOutput::Deferred{});
    drv.env["out"] = "";
    auto drvPath = state.store->writeDerivation(drv);

    // Pre-populate the context accumulator with a Built entry.
    NixStringContext accumulatedContext;
    auto builtEntry = NixStringContextElem{
        NixStringContextElem::Built{
            .drvPath = makeConstantStorePathRef(drvPath),
            .output = "out",
        }};
    accumulatedContext.insert(builtEntry);

    // Coerce a file path through the accumulator.
    auto treePath = state.storePath(treeStorePath);
    auto patchPath = SourcePath(treePath.accessor, treePath.path / "patch.txt");
    auto result = coercePath(patchPath, accumulatedContext);

    // The result is an independent store path.
    auto resultStorePath = state.store->parseStorePath(result);
    EXPECT_NE(resultStorePath, treeStorePath);

    // The accumulator must contain both the original Built entry
    // AND the new Opaque entry.  The Built entry must not be lost
    // or modified.
    EXPECT_GE(accumulatedContext.size(), 2u)
        << "Accumulator must contain original Built + new Opaque";

    bool hasBuilt = false;
    bool hasOpaque = false;
    for (auto & elem : accumulatedContext) {
        if (std::holds_alternative<NixStringContextElem::Built>(elem.raw))
            hasBuilt = true;
        if (auto * opaque = std::get_if<NixStringContextElem::Opaque>(&elem.raw))
            if (opaque->path == resultStorePath)
                hasOpaque = true;
    }
    EXPECT_TRUE(hasBuilt) << "Original Built entry must be preserved";
    EXPECT_TRUE(hasOpaque) << "New Opaque entry must be added";
}

} // namespace nix
