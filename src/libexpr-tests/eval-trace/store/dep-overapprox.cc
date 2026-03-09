#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/nix-binding.hh"

#include <fstream>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Dep over-approximation tests ────────────────────────────────────
//
// The eval trace records Content deps at file-read time, not at
// value-consumption time. When a file is read during evaluation (e.g.,
// as part of a `//` merge or overlay) but no binding from it is accessed,
// the Content dep still exists. If the file changes, verification fails
// even though the accessed values are unaffected.
//
// This is the root cause of unnecessary cold-run re-evaluations:
// aliases.nix is imported (Content dep recorded), merged via `//` into
// the package set, but no alias binding is accessed during GNOME closure
// evaluation. When aliases.nix changes, the Content dep fails with no
// SC override (no bindings accessed → no SC deps recorded).

struct DepOverapproxFixture : TraceStoreFixture
{
    static std::string readFile(const std::filesystem::path & path)
    {
        std::ifstream ifs(path);
        return {std::istreambuf_iterator<char>(ifs),
                std::istreambuf_iterator<char>()};
    }

    void rewriteFile(const std::filesystem::path & target,
                     const TempTestFile & source)
    {
        std::filesystem::copy_file(source.path, target,
            std::filesystem::copy_options::overwrite_existing);
        getFSSourceAccessor()->invalidateCache(CanonPath(target.string()));
        StatHashStore::instance().clear();
    }
};

// ── Over-approximation: Content dep on file whose bindings aren't accessed ──

TEST_F(DepOverapproxFixture, UnusedFileDep_ContentFailsCausesSpuriousMiss)
{
    // Simulates: `{ a = 1; } // import ./aliases.nix`
    // Trace accesses only `a` (from the literal). aliases.nix is read
    // (Content dep) but no binding from it is selected.
    //
    // T1 deps: [Content(main.nix)=hMain, Content(aliases.nix)=hAliases]
    // Result: value of `a` (unrelated to aliases.nix)
    // Change aliases.nix → Content dep fails, no SC coverage → miss.
    // The result SHOULD be unchanged (we only accessed `a`).
    TempTestFile mainFile("main_content_v1");
    TempTestFile aliasesFile("aliases_content_v1");

    auto db = makeDb();

    db.record(rootPath(), string_t{"result_from_a", {}}, {
        makeContentDep(pools(), mainFile.path.string(), "main_content_v1"),
        makeContentDep(pools(), aliasesFile.path.string(), "aliases_content_v1"),
    });

    // Change ONLY aliasesFile (unused dep)
    auto newAliases = TempTestFile("aliases_content_v2");
    rewriteFile(aliasesFile.path, newAliases);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Over-approximation: Content dep on unaccessed file causes miss. "
           "The result depends only on main.nix, not aliases.nix.";
}

TEST_F(DepOverapproxFixture, UsedFileDep_ContentFailsCorrectMiss)
{
    // Contrast: when the accessed file changes, miss is correct.
    TempTestFile mainFile("main_content_v1");
    TempTestFile aliasesFile("aliases_content_v1");

    auto db = makeDb();

    db.record(rootPath(), string_t{"result_from_main", {}}, {
        makeContentDep(pools(), mainFile.path.string(), "main_content_v1"),
        makeContentDep(pools(), aliasesFile.path.string(), "aliases_content_v1"),
    });

    // Change mainFile (the dep that actually matters)
    auto newMain = TempTestFile("main_content_v2");
    rewriteFile(mainFile.path, newMain);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Correct miss: the accessed file changed.";
}

// ── SC coverage partially helps but doesn't solve over-approximation ──

TEST_F(DepOverapproxFixture, SCOnAccessedFile_UnusedFileStillCausesMiss)
{
    // Even with SC coverage on the ACCESSED file, an unaccessed file
    // without SC coverage still causes a miss when it changes.
    //
    // Deps: [Content(main.nix), SC(main.nix, "a"), Content(aliases.nix)]
    // Change aliases.nix → Content(aliases.nix) fails.
    // SC on main.nix covers main.nix, but aliases.nix has no SC → uncovered.
    // Override checks: is aliases.nix covered? No → override rejects → miss.
    TempTestFile mainFile("{ accessed = 1; other = 2; }");
    TempTestFile aliasesFile("unused_aliases_v1");

    auto db = makeDb();

    auto mainContent = readFile(mainFile.path);
    auto aliasesContent = readFile(aliasesFile.path);

    // SC dep on main.nix "accessed" binding (format 'n')
    auto scKey = nlohmann::json{
        {"f", mainFile.path.string()}, {"t", "n"},
        {"p", nlohmann::json::array({"accessed"})}
    }.dump();

    // Need the real binding hash from the eligible file
    auto expr = state.parseExprFromString(
        mainContent, state.rootPath(CanonPath(mainFile.path.string())).parent());
    auto [exprAttrs, scopeExprs] = findNonRecExprAttrs(expr);
    ASSERT_NE(exprAttrs, nullptr);
    auto scopeHash = computeNixScopeHash(scopeExprs, state.symbols);
    std::optional<Blake3Hash> bindingHash;
    for (auto & [sym, def] : *exprAttrs->attrs) {
        if (std::string(state.symbols[sym]) == "accessed") {
            bindingHash = computeNixBindingHash(
                scopeHash, "accessed", static_cast<int>(def.kind),
                def.e, state.symbols);
        }
    }
    ASSERT_TRUE(bindingHash.has_value());

    db.record(rootPath(), string_t{"result_1", {}}, {
        makeContentDep(pools(), mainFile.path.string(), mainContent),
        {{DepType::StructuredContent,
          pools().intern<DepSourceId>(""),
          pools().intern<DepKeyId>(scKey)},
         *bindingHash},
        makeContentDep(pools(), aliasesFile.path.string(), aliasesContent),
    });

    // Change only the UNUSED file
    auto newAliases = TempTestFile("unused_aliases_v2");
    rewriteFile(aliasesFile.path, newAliases);
    db.clearSessionCaches();

    // Content(aliases.nix) fails. aliases.nix has no SC dep → uncovered.
    // Even though main.nix's SC dep passes, the override requires ALL
    // failed Content files to be covered.
    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Unaccessed file with no SC coverage blocks the override, "
           "even though the accessed file's SC dep passes.";
}

TEST_F(DepOverapproxFixture, SCOnBothFiles_UnusedFileChangeOverrideAccepts)
{
    // If BOTH files have SC coverage and the accessed bindings are
    // unchanged, the override accepts. This is the ideal behavior —
    // but requires SC coverage on the unaccessed file, which only
    // happens if some binding from it IS accessed.
    //
    // This test uses two eligible files with NixBinding deps.
    TempTestFile mainFile("{ accessed = 1; }");
    TempTestFile otherFile("{ unrelated = 2; }");

    auto db = makeDb();

    auto mainContent = readFile(mainFile.path);
    auto otherContent = readFile(otherFile.path);

    // Compute real binding hashes for both files
    auto makeScDep = [&](const std::filesystem::path & path,
                         const std::string & content,
                         const std::string & bindingName) -> Dep {
        auto expr = state.parseExprFromString(
            content, state.rootPath(CanonPath(path.string())).parent());
        auto [ea, se] = findNonRecExprAttrs(expr);
        auto sh = computeNixScopeHash(se, state.symbols);
        Blake3Hash bh{};
        for (auto & [sym, def] : *ea->attrs) {
            if (std::string(state.symbols[sym]) == bindingName)
                bh = computeNixBindingHash(sh, bindingName,
                    static_cast<int>(def.kind), def.e, state.symbols);
        }
        auto key = nlohmann::json{
            {"f", path.string()}, {"t", "n"},
            {"p", nlohmann::json::array({bindingName})}
        }.dump();
        return {{DepType::StructuredContent,
                 pools().intern<DepSourceId>(""),
                 pools().intern<DepKeyId>(key)}, bh};
    };

    db.record(rootPath(), string_t{"result", {}}, {
        makeContentDep(pools(), mainFile.path.string(), mainContent),
        makeScDep(mainFile.path, mainContent, "accessed"),
        makeContentDep(pools(), otherFile.path.string(), otherContent),
        makeScDep(otherFile.path, otherContent, "unrelated"),
    });

    // Change the OTHER file's binding value
    auto newOther = TempTestFile("{ unrelated = 999; }");
    rewriteFile(otherFile.path, newOther);
    db.clearSessionCaches();

    // Content(otherFile) fails. SC dep on "unrelated" → re-parse →
    // binding hash changed (999 vs 2) → SC dep FAILS → override rejects.
    // This is correct: the accessed binding from otherFile changed.
    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value())
        << "SC dep on other file correctly detects binding change.";
}

TEST_F(DepOverapproxFixture, SCOnBothFiles_UnrelatedBindingChange_OverrideAccepts)
{
    // Both files have SC coverage. Change a binding in otherFile that
    // is NOT the one the trace depends on → override accepts.
    TempTestFile mainFile("{ accessed = 1; }");
    TempTestFile otherFile("{ tracked = 2; untracked = 3; }");

    auto db = makeDb();

    auto mainContent = readFile(mainFile.path);
    auto otherContent = readFile(otherFile.path);

    auto makeScDep = [&](const std::filesystem::path & path,
                         const std::string & content,
                         const std::string & bindingName) -> Dep {
        auto expr = state.parseExprFromString(
            content, state.rootPath(CanonPath(path.string())).parent());
        auto [ea, se] = findNonRecExprAttrs(expr);
        auto sh = computeNixScopeHash(se, state.symbols);
        Blake3Hash bh{};
        for (auto & [sym, def] : *ea->attrs) {
            if (std::string(state.symbols[sym]) == bindingName)
                bh = computeNixBindingHash(sh, bindingName,
                    static_cast<int>(def.kind), def.e, state.symbols);
        }
        auto key = nlohmann::json{
            {"f", path.string()}, {"t", "n"},
            {"p", nlohmann::json::array({bindingName})}
        }.dump();
        return {{DepType::StructuredContent,
                 pools().intern<DepSourceId>(""),
                 pools().intern<DepKeyId>(key)}, bh};
    };

    db.record(rootPath(), string_t{"result", {}}, {
        makeContentDep(pools(), mainFile.path.string(), mainContent),
        makeScDep(mainFile.path, mainContent, "accessed"),
        makeContentDep(pools(), otherFile.path.string(), otherContent),
        makeScDep(otherFile.path, otherContent, "tracked"),
    });

    // Change only the UNTRACKED binding in otherFile
    auto newOther = TempTestFile("{ tracked = 2; untracked = 999; }");
    rewriteFile(otherFile.path, newOther);
    db.clearSessionCaches();

    // Content(otherFile) fails. SC dep on "tracked" → re-parse →
    // binding hash unchanged → passes. Both failed files covered by
    // passing SC deps → override accepts → hit.
    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value())
        << "Both files have SC coverage, tracked binding unchanged → hit.";
    assertCachedResultEquals(string_t{"result", {}}, result->value, state.symbols);
}

} // namespace nix::eval_trace
