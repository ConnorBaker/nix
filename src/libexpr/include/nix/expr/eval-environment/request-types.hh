#pragma once

#include "nix/expr/eval-environment/capabilities.hh"
#include "nix/expr/eval-environment/domains.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/registry.hh"
#include "nix/store/content-address.hh"
#include "nix/store/path.hh"
#include "nix/util/pos-idx.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-path.hh"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nix {

class EvalState;
struct LookupPath;
struct Value;

enum class PathObservationMode : uint8_t {
    Stat = 0,
    MustBeDirectory,
};

enum class StorePathPublicationMode : uint8_t {
    Preserve = 0,
    Detach,
    Plain,
};

enum class UriPolicyScope : uint8_t {
    General = 0,
    LookupPath,
    FetchInput,
};

enum class LookupPathAccessControlMode : uint8_t {
    Initialize = 0,
    ReuseExisting,
};

enum class StringRealisationMode : uint8_t {
    ImportFromDerivation = 0,
    NonImportFromDerivation,
};

struct CoercedPathRequest
{
    SourcePath path;
    NixStringContext context;
    std::optional<PathObject> origin;

    static CoercedPathRequest coerceFrom(EvalState &, PosIdx, Value &, std::string_view errorCtx);
};

struct PathObservationRequest
{
    CoercedPathRequest coercedPath;
    std::optional<SymlinkResolution> symlinkResolution;
    PathObservationMode mode = PathObservationMode::Stat;

    static PathObservationRequest coerceFrom(
        EvalState &, PosIdx, Value &, std::string_view errorCtx,
        std::optional<SymlinkResolution>, PathObservationMode);
};

struct ReadFileRequest
{
    CoercedPathRequest coercedPath;

    static ReadFileRequest coerceFrom(EvalState &, PosIdx, Value &, std::string_view errorCtx);
};

struct ReadDirectoryRequest
{
    CoercedPathRequest coercedPath;

    static ReadDirectoryRequest coerceFrom(EvalState &, PosIdx, Value &, std::string_view errorCtx);
};

struct StorePathPublishRequest
{
    CoercedPathRequest coercedPath;
    PosIdx pos = noPos;

    static StorePathPublishRequest coerceFrom(EvalState &, PosIdx, Value &, std::string_view errorCtx);
};

struct CopyPathToStoreRequest
{
    std::string name;
    SourcePath path;
    std::optional<PathObject> origin;
    std::function<bool(const SourcePath &)> filterEvaluator;
    ContentAddressMethod method = ContentAddressMethod::Raw::NixArchive;
    std::optional<Hash> expectedHash;
    NixStringContext context;
    PosIdx pos = noPos;

    bool hasFilter() const noexcept { return static_cast<bool>(filterEvaluator); }
};

struct AuthorizedStorePathRenderRequest
{
    StorePath storePath;
    StorePathPublicationMode publicationMode{};
    std::optional<PathObject> provenance;

    static AuthorizedStorePathRenderRequest preserve(StorePath storePath, PathObject provenance)
    {
        return AuthorizedStorePathRenderRequest{
            .storePath = std::move(storePath),
            .publicationMode = StorePathPublicationMode::Preserve,
            .provenance = std::move(provenance),
        };
    }

    static AuthorizedStorePathRenderRequest detach(StorePath storePath)
    {
        return AuthorizedStorePathRenderRequest{
            .storePath = std::move(storePath),
            .publicationMode = StorePathPublicationMode::Detach,
            .provenance = std::nullopt,
        };
    }

    static AuthorizedStorePathRenderRequest plain(StorePath storePath)
    {
        return AuthorizedStorePathRenderRequest{
            .storePath = std::move(storePath),
            .publicationMode = StorePathPublicationMode::Plain,
            .provenance = std::nullopt,
        };
    }
};

struct LookupPathRequest
{
    std::vector<UnrealizedFullLookupPathEntry> lookupPathEntries;
    std::string logicalPath;
    PosIdx pos = noPos;
    LookupPathAccessControlMode accessControlMode = LookupPathAccessControlMode::Initialize;

    static LookupPathRequest fromState(
        EvalState &, std::string_view logicalPath, PosIdx,
        LookupPathAccessControlMode = LookupPathAccessControlMode::Initialize);

    static LookupPathRequest fromLookupPath(
        EvalState &, const LookupPath &, std::string_view logicalPath, PosIdx,
        LookupPathAccessControlMode = LookupPathAccessControlMode::Initialize);

    static LookupPathRequest coerceFrom(
        EvalState &, PosIdx, Value & searchPathValue, std::string_view logicalPath,
        std::string_view errorCtx,
        LookupPathAccessControlMode = LookupPathAccessControlMode::Initialize);
};

struct UriPolicyRequest
{
    std::string uri;
    UriPolicyScope scope = UriPolicyScope::General;
    PosIdx pos = noPos;

    static UriPolicyRequest make(std::string_view uri, UriPolicyScope scope, PosIdx pos = noPos);
};

struct RealiseContextRequest
{
    NixStringContext context;
    StringRealisationMode mode = StringRealisationMode::ImportFromDerivation;

    static RealiseContextRequest make(
        const NixStringContext & context,
        StringRealisationMode mode = StringRealisationMode::ImportFromDerivation);
};

struct FetchIdentityRequest
{
    OriginalFetchInput input;
    fetchers::UseRegistries useRegistries = fetchers::UseRegistries::No;

    static FetchIdentityRequest make(
        fetchers::Input input,
        fetchers::UseRegistries useRegistries = fetchers::UseRegistries::No);
};

struct EnsureMountedStorePathRequest
{
    StorePath storePath;
    std::shared_ptr<const fetchers::Input> lockedInput;
    std::optional<PathObject> provenance;

    static EnsureMountedStorePathRequest make(
        StorePath storePath,
        fetchers::Input lockedInput,
        std::optional<PathObject> provenance = std::nullopt);
};

struct GitIdentityRequest
{
    CoercedPathRequest repoRoot;

    static GitIdentityRequest coerceFrom(EvalState &, PosIdx, Value &, std::string_view errorCtx);
    static GitIdentityRequest fromRepoRoot(SourcePath repoRoot, std::optional<PathObject> origin = std::nullopt);
};

struct DerivedStorePathRequest
{
    CoercedPathRequest sourcePath;
    std::string storeName;

    static DerivedStorePathRequest coerceFrom(
        EvalState &, PosIdx, Value & sourcePathValue, std::string_view errorCtx,
        std::string storeName);
};

} // namespace nix
