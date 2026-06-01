#pragma once
///@file
///
/// Shared helpers for the op-combinator test files. Defined in a header
/// rather than per-file anonymous namespaces because the libfetchers-tests
/// meson build uses unity builds (`unity: on`), which concatenate the .cc
/// files into a single translation unit; per-file `namespace {}` helpers
/// with the same names would then collide.

#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/error.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-accessor.hh"

#include <optional>
#include <set>
#include <string>

namespace nix::testHelpers {

/// Returns a `MakeNotAllowedError` callback that produces a generic
/// `RestrictedPathError`. Test-suite default; production factories use
/// `makeFilteredOutError` from `source-view-factories.cc` which throws
/// `FileNotFound` instead.
inline MakeNotAllowedError makeOpErr()
{
    return
        [](const CanonPath & p) -> RestrictedPathError { return RestrictedPathError("path '%s' filtered", p.abs()); };
}

/// Build a small in-memory tree shared by Restrict/Mask/SoundnessGuard
/// unit tests:
///     /a/x.txt        ("ax")
///     /a/sub/y.txt    ("sy")
///     /b.txt          ("b")
inline ref<MemorySourceAccessor> makeOpFs()
{
    auto m = make_ref<MemorySourceAccessor>();
    m->addFile(CanonPath("a/x.txt"), "ax");
    m->addFile(CanonPath("a/sub/y.txt"), "sy");
    m->addFile(CanonPath("b.txt"), "b");
    return m;
}

/// Plant `paths` as files in `base`, silently dropping conflicts where
/// a parent component is already a Regular file (e.g. `{/c, /c/d}` from
/// random `Arbitrary<std::set<CanonPath>>` would have `/c` planted as a
/// Regular and then `/c/d` would throw `NotADirectory`). Used by all the
/// rapidcheck property tests for Restrict / Mask.
inline void plantOpFiles(MemorySourceAccessor & base, const std::set<CanonPath> & paths)
{
    for (auto & p : paths) {
        if (p.isRoot())
            continue;
        try {
            base.addFile(p, "data");
        } catch (Error &) {
            /* parent is Regular — skip rather than fail. */
        }
    }
}

/// ERROR-CHANNEL test double (L-ErrPreserve). A leaf whose throwing read
/// methods raise a *bespoke* exception type carrying a unique nonce in
/// its message. Lets the error-channel laws assert not merely that
/// "something threw" (Maybe-equivalence) but that the EXACT dynamic type
/// AND the inner accessor's own message survived the combinator unchanged
/// (Either-equivalence — the channel the §6.6 Item-5 regression broke
/// when a Union masked a git workdir's RestrictedPathError as a generic
/// FileNotFound). Defined here (rather than per-file) because the
/// libfetchers-tests build is a unity build (see file header).
///
/// `CustomBespokeError` is a sibling of `FileNotFound` under
/// `SourceAccessorError`, so it is distinguishable from the `FileNotFound`
/// a Union substitutes on a miss, while still being a `SourceAccessorError`.
MakeError(CustomBespokeError, SourceAccessorError);

/// For a single designated path `p`: `maybeLstat(p)` → nullopt (the
/// silent-probe miss that makes a maybeLstat-then-read combinator like
/// Union fall through / mask), while `readFile`/`readLink`/`readDirectory`/
/// `lstat(p)` throw `CustomBespokeError` whose message is DERIVED FROM the
/// captured nonce. The root is a well-formed directory so the accessor is
/// valid to wrap. Paths other than `p` simply miss.
struct ThrowingLeaf : SourceAccessor
{
    CanonPath p;
    std::string nonce;

    ThrowingLeaf(CanonPath p, std::string nonce)
        : p(std::move(p))
        , nonce(std::move(nonce))
    {
        displayPrefix.clear();
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        if (path.isRoot())
            return Stat{.type = tDirectory};
        return std::nullopt; // including at `p`: the silent-probe miss
    }

    void readFile(const CanonPath & path, Sink &, fun<void(uint64_t)>) override
    {
        if (path == p)
            throw CustomBespokeError("bespoke readFile nonce %s", nonce);
        throw FileNotFound("no such path '%s'", showPath(path));
    }

    Stat lstat(const CanonPath & path) override
    {
        if (path == p)
            throw CustomBespokeError("bespoke lstat nonce %s", nonce);
        return SourceAccessor::lstat(path); // throws generic FileNotFound
    }

    std::string readLink(const CanonPath & path) override
    {
        if (path == p)
            throw CustomBespokeError("bespoke readLink nonce %s", nonce);
        throw FileNotFound("no such path '%s'", showPath(path));
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        if (path == p)
            throw CustomBespokeError("bespoke readDirectory nonce %s", nonce);
        throw FileNotFound("no such path '%s'", showPath(path));
    }
};

/// Read-counting leaf (L-DenyOpaque). Counts calls to its read surface so
/// a deny-test can assert the wrapping operator's deny path NEVER consults
/// `next` (consulting it would leak the existence of denied paths). The
/// root is a directory so the accessor is well-formed; `addFile` plants
/// content as usual via the MemorySourceAccessor base.
struct CallCountingLeaf : MemorySourceAccessor
{
    int readFileCalls = 0;
    int readLinkCalls = 0;
    int readDirectoryCalls = 0;
    int lstatCalls = 0;
    int maybeLstatCalls = 0;

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        ++readFileCalls;
        MemorySourceAccessor::readFile(path, sink, sizeCallback);
    }

    std::string readLink(const CanonPath & path) override
    {
        ++readLinkCalls;
        return MemorySourceAccessor::readLink(path);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        ++readDirectoryCalls;
        return MemorySourceAccessor::readDirectory(path);
    }

    Stat lstat(const CanonPath & path) override
    {
        ++lstatCalls;
        return MemorySourceAccessor::lstat(path);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        ++maybeLstatCalls;
        return MemorySourceAccessor::maybeLstat(path);
    }
};

} // namespace nix::testHelpers
