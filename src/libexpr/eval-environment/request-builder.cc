#include "nix/expr/eval-environment/request-types.hh"

#include "nix/expr/eval.hh"
#include "nix/fetchers/fetchers.hh"

namespace nix {

CoercedPathRequest CoercedPathRequest::coerceFrom(
    EvalState & state, PosIdx pos, Value & v, std::string_view errorCtx)
{
    NixStringContext context;
    auto coerced = state.coerceToCoercedPath(pos, v, context, errorCtx);
    return CoercedPathRequest{
        .path = coerced.path(),
        .context = std::move(context),
        .origin = coerced.origin(),
    };
}

PathObservationRequest PathObservationRequest::coerceFrom(
    EvalState & state, PosIdx pos, Value & v, std::string_view errorCtx,
    std::optional<SymlinkResolution> symlinkResolution, PathObservationMode mode)
{
    return PathObservationRequest{
        .coercedPath = CoercedPathRequest::coerceFrom(state, pos, v, errorCtx),
        .symlinkResolution = symlinkResolution,
        .mode = mode,
    };
}

ReadFileRequest ReadFileRequest::coerceFrom(
    EvalState & state, PosIdx pos, Value & v, std::string_view errorCtx)
{
    return ReadFileRequest{
        .coercedPath = CoercedPathRequest::coerceFrom(state, pos, v, errorCtx),
    };
}

ReadDirectoryRequest ReadDirectoryRequest::coerceFrom(
    EvalState & state, PosIdx pos, Value & v, std::string_view errorCtx)
{
    return ReadDirectoryRequest{
        .coercedPath = CoercedPathRequest::coerceFrom(state, pos, v, errorCtx),
    };
}

StorePathPublishRequest StorePathPublishRequest::coerceFrom(
    EvalState & state, PosIdx pos, Value & v, std::string_view errorCtx)
{
    return StorePathPublishRequest{
        .coercedPath = CoercedPathRequest::coerceFrom(state, pos, v, errorCtx),
        .pos = pos,
    };
}

LookupPathRequest LookupPathRequest::fromState(
    EvalState & state, std::string_view logicalPath, PosIdx pos,
    LookupPathAccessControlMode accessControlMode)
{
    return fromLookupPath(state, state.getLookupPath(), logicalPath, pos, accessControlMode);
}

LookupPathRequest LookupPathRequest::fromLookupPath(
    EvalState & state, const LookupPath & lookupPath,
    std::string_view logicalPath, PosIdx pos,
    LookupPathAccessControlMode accessControlMode)
{
    std::vector<UnrealizedFullLookupPathEntry> entries;
    entries.reserve(lookupPath.elements.size());
    for (const auto & elem : lookupPath.elements) {
        entries.push_back(buildLookupPathEntrySpec(
            {{}, elem.path.origin},
            elem.prefix.s.empty()
                ? std::nullopt
                : std::make_optional(LookupPathPrefix{.value = elem.prefix.s}),
            LookupPathRawValue{.value = elem.path.s}));
    }

    return LookupPathRequest{
        .lookupPathEntries = std::move(entries),
        .logicalPath = std::string(logicalPath),
        .pos = pos,
        .accessControlMode = accessControlMode,
    };
}

LookupPathRequest LookupPathRequest::coerceFrom(
    EvalState & state, PosIdx pos, Value & searchPathValue,
    std::string_view logicalPath, std::string_view errorCtx,
    LookupPathAccessControlMode accessControlMode)
{
    state.forceList(searchPathValue, pos, errorCtx);

    std::vector<UnrealizedFullLookupPathEntry> entries;
    entries.reserve(searchPathValue.listSize());

    for (auto value : searchPathValue.listView()) {
        state.forceAttrs(*value, pos, errorCtx);

        std::string prefix;
        if (auto attr = value->attrs()->get(state.s.prefix))
            prefix = state.forceStringNoCtx(*attr->value, pos, errorCtx);

        auto attr = state.getAttr(state.s.path, value->attrs(), errorCtx);

        NixStringContext context;
        std::optional<PathObject> origin;
        if (auto handle = state.lookupSemanticHandle(*attr->value); handle && handle->hasPath())
            origin = handle->path;

        auto rawValue = state.coerceToString(pos, *attr->value, context, errorCtx, false, false, true).toOwned();

        entries.push_back(buildLookupPathEntrySpec(
            {std::move(context), std::move(origin)},
            prefix.empty()
                ? std::nullopt
                : std::make_optional(LookupPathPrefix{.value = std::move(prefix)}),
            LookupPathRawValue{.value = std::move(rawValue)}));
    }

    return LookupPathRequest{
        .lookupPathEntries = std::move(entries),
        .logicalPath = std::string(logicalPath),
        .pos = pos,
        .accessControlMode = accessControlMode,
    };
}

UriPolicyRequest UriPolicyRequest::make(
    std::string_view uri, UriPolicyScope scope, PosIdx pos)
{
    return UriPolicyRequest{
        .uri = std::string(uri),
        .scope = scope,
        .pos = pos,
    };
}

RealiseContextRequest RealiseContextRequest::make(
    const NixStringContext & context, StringRealisationMode mode)
{
    return RealiseContextRequest{
        .context = context,
        .mode = mode,
    };
}

GitIdentityRequest GitIdentityRequest::coerceFrom(
    EvalState & state, PosIdx pos, Value & v, std::string_view errorCtx)
{
    return GitIdentityRequest{
        .repoRoot = CoercedPathRequest::coerceFrom(state, pos, v, errorCtx),
    };
}

GitIdentityRequest GitIdentityRequest::fromRepoRoot(
    SourcePath repoRoot, std::optional<PathObject> origin)
{
    return GitIdentityRequest{
        .repoRoot = CoercedPathRequest{
            .path = std::move(repoRoot),
            .context = {},
            .origin = std::move(origin),
        },
    };
}

DerivedStorePathRequest DerivedStorePathRequest::coerceFrom(
    EvalState & state, PosIdx pos, Value & sourcePathValue,
    std::string_view errorCtx, std::string storeName)
{
    return DerivedStorePathRequest{
        .sourcePath = CoercedPathRequest::coerceFrom(state, pos, sourcePathValue, errorCtx),
        .storeName = std::move(storeName),
    };
}

FetchIdentityRequest FetchIdentityRequest::make(
    fetchers::Input input, fetchers::UseRegistries useRegistries)
{
    return FetchIdentityRequest{
        .input = OriginalFetchInput{std::make_shared<const fetchers::Input>(std::move(input))},
        .useRegistries = useRegistries,
    };
}

EnsureMountedStorePathRequest EnsureMountedStorePathRequest::make(
    StorePath storePath,
    fetchers::Input lockedInput,
    std::optional<PathObject> provenance)
{
    return EnsureMountedStorePathRequest{
        .storePath = std::move(storePath),
        .lockedInput = std::make_shared<const fetchers::Input>(std::move(lockedInput)),
        .provenance = std::move(provenance),
    };
}

} // namespace nix
