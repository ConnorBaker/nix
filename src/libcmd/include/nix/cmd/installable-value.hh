#pragma once
///@file

#include "nix/cmd/installables.hh"
#include "nix/flake/flake.hh"

namespace nix {

struct PackageInfo;
struct SourceExprCommand;

namespace eval_trace {
class TraceCache;
} // namespace eval_trace

struct App
{
    std::vector<DerivedPath> context;
    std::filesystem::path program;
    // FIXME: add args, sandbox settings, metadata, ...
};

struct UnresolvedApp
{
    App unresolved;
    std::vector<BuiltPathWithResult> build(ref<Store> evalStore, ref<Store> store);
    App resolve(ref<Store> evalStore, ref<Store> store);
};

/**
 * Extra info about a \ref DerivedPath "derived path" that ultimately
 * come from a Nix language value.
 *
 * Invariant: every ExtraPathInfo gotten from an InstallableValue should
 * be possible to downcast to an ExtraPathInfoValue.
 */
struct ExtraPathInfoValue : ExtraPathInfo
{
    /**
     * Extra struct to get around C++ designated initializer limitations
     */
    struct Value
    {
        /**
         * An optional priority for use with "build envs". See Package
         */
        std::optional<NixInt::Inner> priority;

        /**
         * The attribute path associated with this value. The idea is
         * that an installable referring to a value typically refers to
         * a larger value, from which we project a smaller value out
         * with this.
         */
        std::string attrPath;

        /**
         * \todo merge with DerivedPath's 'outputs' field?
         */
        ExtendedOutputsSpec extendedOutputsSpec;
    };

    Value value;

    ExtraPathInfoValue(Value && v)
        : value(std::move(v))
    {
    }

    virtual ~ExtraPathInfoValue() = default;
};

/**
 * An Installable which corresponds a Nix language value, in addition to
 * a collection of \ref DerivedPath "derived paths".
 */
struct InstallableValue : Installable
{
    ref<EvalState> state;

    InstallableValue(ref<EvalState> state)
        : state(state)
    {
    }

    virtual ~InstallableValue() {}

    virtual std::pair<Value *, PosIdx> toValue(EvalState & state) = 0;

    /**
     * The resolved attribute path used to reach this value in the eval tree.
     * For flake installables, this is the full path after prefix resolution
     * (e.g., "packages.x86_64-linux.hello"). Empty for non-flake installables.
     */
    virtual std::string resolvedAttrPath() const { return ""; }

    UnresolvedApp toApp(EvalState & state);

    static InstallableValue & require(Installable & installable);
    static ref<InstallableValue> require(ref<Installable> installable);

protected:

    /**
     * Handles either a plain path, or a string with a single string
     * context elem in the right format. The latter case is handled by
     * `EvalState::coerceToDerivedPath()`; see it for details.
     *
     * @param v Value that is hopefully a string or path per the above.
     *
     * @param pos Position of value to aid with diagnostics.
     *
     * @param errorCtx Arbitrary message for use in potential error message when something is wrong with `v`.
     *
     * @result A derived path (with empty info, for now) if the value
     * matched the above criteria.
     */
    std::optional<DerivedPathWithInfo>
    trySinglePathToDerivedPaths(Value & v, const PosIdx pos, std::string_view errorCtx);
};

} // namespace nix
