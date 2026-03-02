#include "nix/cmd/installable-value.hh"
#include "nix/fetchers/fetch-to-store.hh"

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
        auto storePath = fetchToStore(state->fetchSettings, *state->store, v.path(), FetchMode::Copy);
        return {{
            .path =
                DerivedPath::Opaque{
                    .path = std::move(storePath),
                },
            .info = make_ref<ExtraPathInfo>(),
        }};
    }

    else if (v.type() == nString) {
        return {{
            .path = DerivedPath::fromSingle(state->coerceToSingleDerivedPath(pos, v, errorCtx)),
            .info = make_ref<ExtraPathInfo>(),
        }};
    }

    else
        return std::nullopt;
}

} // namespace nix
