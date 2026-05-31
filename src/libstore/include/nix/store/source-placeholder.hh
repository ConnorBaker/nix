#pragma once
///@file
///
/// `SourcePlaceholder` — a deterministic, opaque, content-keyed
/// string standing in for a source-store-path while its bytes
/// haven't been materialised yet.
///
/// Mirrors `DownstreamPlaceholder` (build-side floating CA) for
/// source-side floating CA. Two `builtins.path` calls anywhere in a
/// codebase with the same `SourceContentId` and `name` produce the
/// same `SourcePlaceholder` string. A cargo workspace's 200 packages
/// all sharing one source + filter share one `SourceContentId`, so the
/// scheduler runs **one shared narHash walk** for the whole workspace
/// (coalesced by contentId). The per-package byte-copies are NOT shared
/// on a cold store — distinct `name`s give distinct CA store paths — so
/// "one walk" means the shared narHash computation, not one ingestion.
/// See `doc/tecnix-survey/PROPOSAL.md` §4.
///
/// The placeholder renders as `/<52 base32 chars>` (SHA-256 in Nix32;
/// no `-name` suffix), matching `DownstreamPlaceholder::render()`. It is
/// intentionally *not* shaped like a real store path so that:
///
///   1. The scheduler can detect placeholder strings in
///      derivation env / inputSrcs and rewrite them to real paths
///      without ambiguity.
///   2. Error messages mentioning unrealised placeholders quote
///      *the same string every run* — no DetSys-style random hashes.
///      Bug reports reproduce.

#include "nix/store/source-content-id.hh"
#include "nix/util/hash.hh"

#include <string>
#include <string_view>

namespace nix {

class SourcePlaceholder
{
    Hash hash;

    explicit SourcePlaceholder(Hash h)
        : hash(std::move(h))
    {
    }

public:
    /** Construct from a content id + name. The name is the only
     *  per-call distinguishing input; everything else is shared via
     *  `contentId`. */
    static SourcePlaceholder make(const SourceContentId & contentId, std::string_view name);

    /** Render to the wire-format string. Opaque; not a store path. */
    std::string render() const;

    /** Try to parse a render result back. Returns nullopt if the
     *  string isn't shaped like a placeholder. */
    static std::optional<SourcePlaceholder> tryParse(std::string_view);

    bool operator==(const SourcePlaceholder &) const = default;
    auto operator<=>(const SourcePlaceholder &) const = default;

    const Hash & getHash() const
    {
        return hash;
    }
};

inline std::size_t hash_value(const SourcePlaceholder & p)
{
    std::size_t result;
    std::memcpy(&result, p.getHash().hash, sizeof(result));
    return result;
}

} // namespace nix

template<>
struct std::hash<nix::SourcePlaceholder>
{
    std::size_t operator()(const nix::SourcePlaceholder & p) const noexcept
    {
        return nix::hash_value(p);
    }
};
