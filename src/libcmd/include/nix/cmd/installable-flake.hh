#pragma once
///@file

#include "nix/cmd/common-eval-args.hh"
#include "nix/cmd/installable-value.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"

namespace nix {

/**
 * Extra info about a \ref DerivedPath "derived path" that ultimately
 * come from a Flake.
 *
 * Invariant: every ExtraPathInfo gotten from an InstallableFlake should
 * be possible to downcast to an ExtraPathInfoFlake.
 */
struct ExtraPathInfoFlake : ExtraPathInfoValue
{
    /**
     * Extra struct to get around C++ designated initializer limitations
     */
    struct Flake
    {
        FlakeRef originalRef;
        FlakeRef lockedRef;
    };

    Flake flake;

    ExtraPathInfoFlake(Value && v, Flake && f)
        : ExtraPathInfoValue(std::move(v))
        , flake(std::move(f))
    {
    }
};

struct InstallableFlake : InstallableValue
{
    FlakeRef flakeRef;
    Strings attrPaths;
    Strings prefixes;
    ExtendedOutputsSpec extendedOutputsSpec;
    const flake::LockFlags & lockFlags;
    mutable std::shared_ptr<flake::LockedFlake> _lockedFlake;
    mutable std::string resolvedAttrPath_;

    InstallableFlake(
        SourceExprCommand * cmd,
        ref<EvalState> state,
        FlakeRef && flakeRef,
        std::string_view fragment,
        ExtendedOutputsSpec extendedOutputsSpec,
        Strings attrPaths,
        Strings prefixes,
        const flake::LockFlags & lockFlags);

    std::string what() const override
    {
        if (attrPaths.size() == 1 && attrPaths.front().starts_with("."))
            return flakeRef.to_string() + "#" + attrPaths.front().substr(1);
        return flakeRef.to_string() + "#" + *attrPaths.begin();
    }

    std::vector<std::string> getActualAttrPaths();

    DerivedPathsWithInfo toDerivedPaths() override;

    EvaluatedInstallableValue toValue(EvalState & state) override;

    std::string resolvedAttrPath() const override { return resolvedAttrPath_; }

    ref<flake::LockedFlake> getLockedFlake() const;
    ref<eval_trace::TraceSession> getOrCreateTraceCache(EvalState & state) const override;

    FlakeRef nixpkgsFlakeRef() const;
};

/**
 * Default flake ref for referring to Nixpkgs. For flakes that don't
 * have their own Nixpkgs input, or other installables.
 *
 * It is a layer violation for Nix to know about Nixpkgs; currently just
 * `nix develop` does. Be wary of using this /
 * `InstallableFlake::nixpkgsFlakeRef` more places.
 */
static inline FlakeRef defaultNixpkgsFlakeRef()
{
    return FlakeRef::fromAttrs(fetchSettings, {{"type", "indirect"}, {"id", "nixpkgs"}});
}

} // namespace nix
