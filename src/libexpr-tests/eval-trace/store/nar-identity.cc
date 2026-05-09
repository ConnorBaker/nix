/**
 * A-2 · NarIdentity verify + invalidation
 *
 * Tests for the NarIdentity (CQK::NarIdentity) dep kind — a NAR-level hash
 * of a path (file or directory) that detects any content change, including
 * permission bits and symlink structure.
 *
 * Because NarIdentity verification calls depHashPath(sourcePath),
 * the recorded hash must be computed the same way.
 *
 * Note: computeNarHash is implemented inline here as a call to depHashPath()
 * with a SourcePath wrapper.  If a shared computeNarHash helper is added to
 * helpers.hh in the future, switch to that.
 */

#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/source-accessor.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Local helper: compute the NAR hash for a filesystem path ─────────
//
// Mirrors the verifier's runtime call to depHashPath(sourcePath).

static DepHash computeNarHash(const std::filesystem::path & p)
{
    auto sp = SourcePath(getFSSourceAccessor(), CanonPath(p.string()));
    return depHashPath(sp);
}

// ── A-2: NarIdentity_RecordVerify_WarmHit ────────────────────────────

TEST_F(TraceStoreTest, NarIdentity_RecordVerify_WarmHit)
{
    TempDir td;
    td.addFile("x", "c");

    auto db  = makeDb();
    auto key = td.path().string();

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}},
                   {makeNARContentDep(pools(), key, computeNarHash(td.path()))});
    });

    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_TRUE(result.has_value());
}

// ── A-2: NarIdentity_HashChange_Invalidation ─────────────────────────
//
// Use a single FILE (not a directory) as the NarIdentity dep target.
//
// Rationale: use a single file, not a directory. Overwriting a file ALWAYS
// changes the file's own mtime, ensuring the NAR hash is recomputed from
// new content when the verifier re-reads it.

TEST_F(TraceStoreTest, NarIdentity_HashChange_Invalidation)
{
    // Use a single file, not a directory.  File mtime changes on overwrite.
    TempTextFile f("original");
    auto filePath = std::filesystem::canonical(f.path).string();
    auto canonPath = std::filesystem::path(filePath);

    auto db  = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}},
                   {makeNARContentDep(pools(), filePath, computeNarHash(canonPath))});
    });

    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        EXPECT_TRUE(result.has_value());
    }

    // Overwrite the file to change the NAR hash.  The file's own mtime
    // changes, invalidating the file cache for the file path.
    f.modify("changed");
    getFSSourceAccessor()->invalidateCache();

    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        EXPECT_FALSE(result.has_value());
    }
}

} // namespace nix::eval_trace
