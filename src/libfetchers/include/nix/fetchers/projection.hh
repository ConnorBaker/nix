#pragma once
///@file
///
/// `Projection` — content-keyed cache discipline as a CRTP type.
///
/// The fetcher cache holds many `(domain, key) → value` lookups that all
/// share the same shape: a key derived from content, a deterministic
/// compute, an encoded value persisted under that key. `Projection`
/// names that shape so we don't repeat it. It is a refactor anchor, not
/// a new runtime layer.
///
/// A concrete projection supplies:
///
/// ```cpp
/// struct Foo : Projection<Foo, From, To> {
///     static constexpr std::string_view domain = "foo";
///     static Attrs toKey(const From &);
///     static To fromValue(const Attrs &);
///     static Attrs toValue(const To &);
/// };
/// ```
///
/// and calls `Foo::lookup(settings, from, [&]{ ...compute... })`.
/// `compute()` runs only on a miss; its result is upserted under the
/// canonical key.
///
/// Soundness law: equal `(domain, toKey(from))` ⇒ equal `toValue(...)`.
/// Domains that violate this (TTL, store-GC, etc.) use the bespoke
/// `Cache::lookupWithTTL` / `lookupStorePath` paths instead.

#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-settings.hh"

namespace nix::fetchers {

template<typename Derived, typename From, typename To>
struct Projection
{
    static Cache::Key key(const From & from)
    {
        return {Derived::domain, Derived::toKey(from)};
    }

    static std::optional<To> peek(const Settings & settings, const From & from)
    {
        if (auto cached = settings.getCache()->lookup(key(from)))
            return Derived::fromValue(*cached);
        return std::nullopt;
    }

    template<typename Compute>
    static To lookup(const Settings & settings, const From & from, Compute && compute)
    {
        if (auto cached = peek(settings, from))
            return std::move(*cached);
        To result = std::forward<Compute>(compute)();
        settings.getCache()->upsert(key(from), Derived::toValue(result));
        return result;
    }
};

/* --- Concrete projection instances --- */

/**
 * Tree SHA → NAR hash. Content-keyed: equal Git tree OIDs serialise to
 * equal NARs (modulo NAR-canonicalisation version, which the proposal
 * notes as out-of-scope drift).
 *
 * Already populated by the tarball / GitHub fetchers; consulted by the
 * subtree-aware fingerprint bridge in fetch-to-store.
 */
struct TreeHashToNarHash : Projection<TreeHashToNarHash, Hash, Hash>
{
    static constexpr std::string_view domain = "treeHashToNarHash";

    static Attrs toKey(const Hash & treeHash)
    {
        return {{"treeHash", treeHash.gitRev()}};
    }

    static Hash fromValue(const Attrs & attrs)
    {
        /* `toValue` writes SRI, so decode with `parseSRI` — consistent
           with the three sibling NAR-hash projections
           (FilteredSourcePathToHash / SourcePathToHash /
           SourceContentToNarHash). The previous `parseAny(…, SHA256)`
           accepted the SRI form too, but was gratuitously looser than
           what the encoder produces. */
        return Hash::parseSRI(getStrAttr(attrs, "narHash"));
    }

    static Attrs toValue(const Hash & narHash)
    {
        return {{"narHash", narHash.to_string(HashFormat::SRI, true)}};
    }
};

/**
 * (rev) → revcount. Per-rev metadata; content-determined.
 */
struct GitRevCount : Projection<GitRevCount, Hash, uint64_t>
{
    static constexpr std::string_view domain = "gitRevCount";

    static Attrs toKey(const Hash & rev)
    {
        return {{"rev", rev.gitRev()}};
    }

    static uint64_t fromValue(const Attrs & attrs)
    {
        return getIntAttr(attrs, "revCount");
    }

    static Attrs toValue(const uint64_t & revCount)
    {
        return {{"revCount", revCount}};
    }
};

/**
 * Filtered-source projection key.
 *
 * `(sourceFingerprint, method, root, shapeHash) → narHash`.
 *
 * `shapeHash` digests the *post-filter* canonical NAR shape (paths,
 * types, executable bits, symlink targets — no file contents). The
 * source fingerprint is required: equal shape over different sources is
 * obviously not equal output. Two filters whose accepted set produces
 * the same shape against the same source share a row — that's the
 * "filter identity is undecidable but filter shape is observable" win.
 */
struct FilteredSourceKey
{
    std::string sourceFingerprint;
    std::string method;
    std::string subpath;
    std::string shapeHash;
    auto operator<=>(const FilteredSourceKey &) const = default;
};

struct FilteredSourcePathToHash : Projection<FilteredSourcePathToHash, FilteredSourceKey, Hash>
{
    static constexpr std::string_view domain = "filteredSourcePathToHash";

    static Attrs toKey(const FilteredSourceKey & k)
    {
        return {
            {"fingerprint", k.sourceFingerprint},
            {"method", k.method},
            {"path", k.subpath},
            {"shape", k.shapeHash},
        };
    }

    static Hash fromValue(const Attrs & attrs)
    {
        return Hash::parseSRI(getStrAttr(attrs, "hash"));
    }

    static Attrs toValue(const Hash & h)
    {
        return {{"hash", h.to_string(HashFormat::SRI, true)}};
    }
};

/**
 * Unfiltered source projection key.
 *
 * `(sourceFingerprint, method, subpath) → narHash`.
 *
 * The no-filter sibling of `FilteredSourcePathToHash`: a whole source
 * (or subtree) addressed by its fingerprint, with no post-filter shape.
 * Consulted/populated by `fetchToStore2`'s unfiltered branch.
 */
struct SourcePathKey
{
    std::string sourceFingerprint;
    std::string method;
    std::string subpath;
    auto operator<=>(const SourcePathKey &) const = default;
};

struct SourcePathToHash : Projection<SourcePathToHash, SourcePathKey, Hash>
{
    static constexpr std::string_view domain = "sourcePathToHash";

    static Attrs toKey(const SourcePathKey & k)
    {
        return {
            {"fingerprint", k.sourceFingerprint},
            {"method", k.method},
            {"path", k.subpath},
        };
    }

    static Hash fromValue(const Attrs & attrs)
    {
        return Hash::parseSRI(getStrAttr(attrs, "hash"));
    }

    static Attrs toValue(const Hash & h)
    {
        return {{"hash", h.to_string(HashFormat::SRI, true)}};
    }
};

/**
 * `SourceContentId` → NAR hash.
 *
 * The eval-side realisation registry (`MaterialisationScheduler`)
 * persists content-keyed walk results here: two `builtins.path` calls
 * anywhere with the same content+filter inputs share one row, hence one
 * walk across processes. Keyed on the `SourceContentId`'s string form.
 */
struct SourceContentToNarHash : Projection<SourceContentToNarHash, std::string, Hash>
{
    static constexpr std::string_view domain = "sourceContentToNarHash";

    static Attrs toKey(const std::string & contentId)
    {
        return {{"contentId", contentId}};
    }

    static Hash fromValue(const Attrs & attrs)
    {
        return Hash::parseSRI(getStrAttr(attrs, "narHash"));
    }

    static Attrs toValue(const Hash & narHash)
    {
        return {{"narHash", narHash.to_string(HashFormat::SRI, true)}};
    }
};

/**
 * (rev) → commit time. Content-determined: a Git commit's author time
 * is in the bytes that compose the commit OID.
 */
struct GitLastModified : Projection<GitLastModified, Hash, uint64_t>
{
    static constexpr std::string_view domain = "gitLastModified";

    static Attrs toKey(const Hash & rev)
    {
        return {{"rev", rev.gitRev()}};
    }

    static uint64_t fromValue(const Attrs & attrs)
    {
        return getIntAttr(attrs, "lastModified");
    }

    static Attrs toValue(const uint64_t & t)
    {
        return {{"lastModified", t}};
    }
};

} // namespace nix::fetchers
