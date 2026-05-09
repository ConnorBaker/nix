#pragma once
/// parse-caches.hh — DOM/path caches for dep hash verification.
///
/// Owned by VerificationSession (accessed via StrandToken<FileStrandTag>).
/// Caches parsed JSON/TOML/directory DOMs and resolved SourcePaths to avoid
/// re-parsing and re-resolving during one verification state. All access
/// requires a StrandToken<FileStrandTag>, enforcing that these caches are only
/// used from the correct execution context.

#include "nix/expr/eval-trace/deps/nix-binding.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/strand-local.hh"
#include "nix/util/source-path.hh"
#include "nix/util/source-accessor.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <nlohmann/json.hpp>
#include <toml.hpp>

#include <optional>
#include <string>
#include <unordered_map>

namespace nix::eval_trace {

/// Interned cache key for DOM/SourcePath caches.
struct FileCacheKey {
    DepSourceId sourceId;
    FilePathId filePathId;

    bool operator==(const FileCacheKey &) const = default;

    struct Hash {
        // No `is_avalanching` marker.  `hashValues` over identity-hashed
        // uint32_t fields does not avalanche.
        size_t operator()(const FileCacheKey & k) const noexcept
        {
            return hashValues(k.sourceId.value, k.filePathId.value);
        }
    };
};

struct StructuredSourceCacheKey {
    FileCacheKey file;
    StructuredFormat format;

    bool operator==(const StructuredSourceCacheKey &) const = default;

    struct Hash {
        size_t operator()(const StructuredSourceCacheKey & k) const noexcept
        {
            return hashValues(
                k.file.sourceId.value,
                k.file.filePathId.value,
                static_cast<uint8_t>(k.format));
        }
    };
};

/// Parsed DOM and resolved path caches for verification.
/// Each cache is wrapped in StrandLocal<..., FileStrandTag> to enforce
/// that access only happens with a FileStrandTag token.
struct ParseCaches {
    using JsonDomCache = boost::unordered_flat_map<FileCacheKey, nlohmann::json, FileCacheKey::Hash>;
    using TomlDomCache = boost::unordered_flat_map<FileCacheKey, toml::value, FileCacheKey::Hash>;
    using DirListingCache = boost::unordered_flat_map<FileCacheKey, SourceAccessor::DirEntries, FileCacheKey::Hash>;
    using NixAstBindings = std::unordered_map<std::string, NixBindingHash>;
    using NixAstCache = boost::unordered_flat_map<FileCacheKey, NixAstBindings, FileCacheKey::Hash>;
    using SourcePathCache = boost::unordered_flat_map<FileCacheKey, std::optional<SourcePath>, FileCacheKey::Hash>;
    using StructuredSourceFailureCache = boost::unordered_flat_map<
        StructuredSourceCacheKey,
        std::optional<DepHashValue>,
        StructuredSourceCacheKey::Hash>;

    StrandLocal<JsonDomCache, FileStrandTag> jsonDomCache;
    StrandLocal<TomlDomCache, FileStrandTag> tomlDomCache;
    StrandLocal<DirListingCache, FileStrandTag> dirListingCache;
    StrandLocal<NixAstCache, FileStrandTag> nixAstCache;
    StrandLocal<SourcePathCache, FileStrandTag> sourcePathCache;
    StrandLocal<StructuredSourceFailureCache, FileStrandTag> structuredSourceFailureCache;
};

} // namespace nix::eval_trace
