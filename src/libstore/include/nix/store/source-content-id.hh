#pragma once
///@file
///
/// `SourceContentId` — a content-determined identity for a piece of
/// source material destined to land in the store.
///
/// Computed from inputs that are known *without walking the source*:
///
///   - `sourceFingerprint`: the underlying source's content fingerprint
///     (Track B's subpath-aware `getFingerprint` already provides this).
///   - `shapeHash`: digest of the post-filter accepted shape (Track D's
///     `collectFilteredShape`; metadata-only walk, no blob reads).
///   - `method`: `ContentAddressMethod` (NAR / Git / Flat).
///   - `refs`: store-path references that flow through.
///
/// **Critically: no call-site identity.** Two `builtins.path` calls
/// anywhere in a codebase with the same inputs produce the same
/// `SourceContentId`. A cargo workspace with 200 packages all
/// referencing the same source + filter shares one contentId — and
/// therefore one walk, one NAR hash, one persistent cache row.
///
/// The realisation registry (`MaterialisationScheduler`) is keyed by
/// `SourceContentId` for the expensive operation (NAR-hash). The
/// per-call-site `SourcePlaceholder` (which differs by `name`) maps
/// to the resulting storePath via cheap `makeFixedOutputPathFromCA`.

#include "nix/store/content-address.hh"
#include "nix/util/hash.hh"

#include <string>
#include <string_view>

namespace nix {

struct SourceContentId
{
    /** SHA-256 of the canonical encoding. Full 32 bytes — no
     *  truncation, no compression. Used as a cache primary key
     *  where collision resistance matters more than length. */
    Hash hash;

    /** Compute from content-determining inputs. */
    static SourceContentId compute(
        std::string_view sourceFingerprint,
        const Hash & shapeHash,
        ContentAddressMethod method,
        const StoreReferences & refs);

    /** Stable string form for use in cache keys, error messages. */
    std::string to_string() const;

    bool operator==(const SourceContentId &) const = default;
    auto operator<=>(const SourceContentId &) const = default;
};

/** Boost-style ADL hash for use with `boost::concurrent_flat_map`. */
inline std::size_t hash_value(const SourceContentId & id)
{
    std::size_t result;
    std::memcpy(&result, id.hash.hash, sizeof(result));
    return result;
}

} // namespace nix

template<>
struct std::hash<nix::SourceContentId>
{
    std::size_t operator()(const nix::SourceContentId & id) const noexcept
    {
        return nix::hash_value(id);
    }
};
