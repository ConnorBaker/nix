#include "boundary-audit-fixture.hh"

#include "nix/util/hash.hh"

namespace nix {

/**
 * Bypass-site boundary 11: `builtins.hashString`.
 *
 * `src/libexpr/primops.cc:4905-4918`'s `prim_hashString`:
 *
 *     NixStringContext context; // discarded
 *     auto s = state.forceString(*args[1], context, pos, ...);
 *     v.mkString(hashString(*ha, s).to_string(HashFormat::Base16, false), state.mem);
 *
 * Context is read for the side effect of validating the string but
 * is discarded. After Item 1 activation, if `args[1]` carries a
 * `SourceVirtual` placeholder in its context, the body string `s`
 * is the placeholder render `/<base32>`, and the hash result is
 * `hash("/<base32>")` — not `hash(realStorePath)`. Two evaluations
 * of the same source produce the same wrong hash; one evaluation
 * pre-activation produces a different hash than post-activation.
 *
 * DetSys hit this in production (commit 823f756ff, Feb 2026,
 * fixes determinate#160).
 *
 * Cover-fix shape: resolve context, `rewriteStrings` over the body,
 * hash the rewritten body. Test asserts the hash matches what
 * `hashString` would produce on the resolved storepath text — i.e.
 * the same hash as if `args[1]` had been an `Opaque{realPath}`-context
 * value with body = `printStorePath(realPath)`.
 */
class BoundaryAuditBuiltinsHashStringTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditBuiltinsHashStringTest, NoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Replay prim_hashString's body with the cover-fix sequence. */
    NixStringContext context;
    Hash actualHash{HashAlgorithm::SHA256};
    try {
        auto s = state.forceString(v, context, noPos, "while evaluating hashString input");
        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        auto rewritten = rewriteStrings(std::string{s}, rewrites);
        actualHash = hashString(HashAlgorithm::SHA256, rewritten);
    } catch (Error & e) {
        recordThrow("builtins.hashString", e.what());
        FAIL() << "hashString threw: " << e.what();
        return;
    }

    /* Ground truth: the resolved storepath, then hash it. The
       audit assertion is that `actualHash` equals
       `hashString(SHA-256, resolved-storepath-text)` — proving
       that the cover-fix produced the same hash as if the input
       had never been virtual. */
    auto resolvedStorePath = state.materialisationScheduler->outPathOf(placeholder);
    auto expectedBody = state.store->printStorePath(resolvedStorePath);
    auto expectedHash = hashString(HashAlgorithm::SHA256, expectedBody);

    bool match = actualHash == expectedHash;
    if (match) {
        recordPass("builtins.hashString");
    } else {
        std::cerr << "LEAK:  builtins.hashString — hash of placeholder body, not resolved path\n"
                  << "  expected hash: " << expectedHash.to_string(HashFormat::Base16, false) << "\n"
                  << "  actual hash:   " << actualHash.to_string(HashFormat::Base16, false) << "\n";
    }
    EXPECT_EQ(actualHash, expectedHash)
        << "builtins.hashString hashed the placeholder body, not the resolved storepath";
}

} // namespace nix
