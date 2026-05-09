#include "nix/cmd/installable-value.hh"
#include "nix/expr/eval-environment/authority-internal.hh"
#include "nix/expr/eval-environment/environment.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"

namespace nix {

static UsageError nonValueInstallable(Installable & installable)
{
    return UsageError("installable '%s' does not correspond to a Nix language value", installable.what());
}

InstallableValue & InstallableValue::require(Installable & installable)
{
    auto * castedInstallable = dynamic_cast<InstallableValue *>(&installable);
    if (!castedInstallable)
        throw nonValueInstallable(installable);
    return *castedInstallable;
}

ref<InstallableValue> InstallableValue::require(ref<Installable> installable)
{
    auto castedInstallable = installable.dynamic_pointer_cast<InstallableValue>();
    if (!castedInstallable)
        throw nonValueInstallable(*installable);
    return ref{castedInstallable};
}

std::optional<DerivedPathWithInfo>
InstallableValue::trySinglePathToDerivedPaths(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    if (v.type() == nPath) {
        EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(*state));
        auto path = v.path();
        std::optional<PathObject> origin;
        if (auto handle = state->lookupSemanticHandle(v); handle && handle->hasPath())
            origin = handle->path;
        auto request = CopyPathToStoreRequest{
            .name = std::string(path.baseName()),
            .path = path,
            .origin = std::move(origin),
            .filterEvaluator = std::function<bool(const SourcePath &)>(),
            .method = ContentAddressMethod::Raw::NixArchive,
            .expectedHash = std::nullopt,
            .context = {},
            .pos = noPos,
        };
        auto published = environment.copyPathToStore(request);
                return {{
            .path =
                DerivedPath::Opaque{
                    .path = published.storePath(),
                },
            .info = make_ref<ExtraPathInfo>(),
        }};
    }

    else if (v.type() == nString) {
        auto path = state->coerceToSingleDerivedPath(pos, v, errorCtx);
        if (auto o = std::get_if<SingleDerivedPath::Opaque>(&path.raw()))
            state->ensureLazyPathCopied(o->path);
        return {{
            .path = DerivedPath::fromSingle(path),
            .info = make_ref<ExtraPathInfo>(),
        }};
    }

    else
        return std::nullopt;
}

} // namespace nix
