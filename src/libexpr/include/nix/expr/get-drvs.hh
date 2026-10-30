#pragma once
///@file

#include "nix/expr/eval.hh"
#include "nix/store/path.hh"

#include <string>
#include <map>

namespace nix {

/**
 * A "parsed" package attribute set.
 */
struct PackageInfo
{
public:
    typedef std::map<std::string, std::optional<StorePath>, std::less<>> Outputs;

private:
    EvalState * state;

    mutable std::string name;
    mutable std::string system;
    mutable std::optional<std::optional<StorePath>> drvPath;
    mutable std::optional<StorePath> outPath;
    mutable std::string outputName;
    Outputs outputs;

    /**
     * Set if we get an AssertionError
     */
    bool failed = false;

    const Bindings *attrs = nullptr, *meta = nullptr;

    /* Holds `attrs` across an unforce job's end. */
    friend struct DerivationsSeen;

    const Bindings * getMeta();

    bool checkMeta(Value & v);

public:
    /**
     * path towards the derivation
     */
    std::string attrPath;

    PackageInfo(EvalState & state)
        : state(&state) {};
    PackageInfo(EvalState & state, std::string attrPath, const Bindings * attrs);
    PackageInfo(EvalState & state, ref<Store> store, const std::string & drvPathWithOutputs);

    std::string queryName() const;
    std::string querySystem() const;
    std::optional<StorePath> queryDrvPath() const;
    StorePath requireDrvPath() const;
    StorePath queryOutPath() const;
    std::string queryOutputName() const;
    /**
     * Return the unordered map of output names to (optional) output paths.
     * The "outputs to install" are determined by `meta.outputsToInstall`.
     */
    Outputs queryOutputs(bool withPaths = true, bool onlyOutputsToInstall = false);

    StringSet queryMetaNames();
    Value * queryMeta(const std::string & name);
    std::string queryMetaString(const std::string & name);
    NixInt queryMetaInt(const std::string & name, NixInt def);
    NixFloat queryMetaFloat(const std::string & name, NixFloat def);
    bool queryMetaBool(const std::string & name, bool def);
    void setMeta(const std::string & name, Value * v);

    /*
    MetaInfo queryMetaInfo(EvalState & state) const;
    MetaValue queryMetaInfo(EvalState & state, const string & name) const;
    */

    void setName(const std::string & s)
    {
        name = s;
    }

    void setDrvPath(StorePath path)
    {
        drvPath = {{std::move(path)}};
    }

    void setOutPath(StorePath path)
    {
        outPath = {{std::move(path)}};
    }

    void setFailed()
    {
        failed = true;
    };

    bool hasFailed()
    {
        return failed;
    };
};

typedef std::list<PackageInfo, traceable_allocator<PackageInfo>> PackageInfos;

/**
 * If value `v` denotes a derivation, return a PackageInfo object
 * describing it. Otherwise return nothing.
 */
std::optional<PackageInfo> getDerivation(EvalState & state, Value & v, bool ignoreAssertionFailures);

/**
 * If value `v` denotes a derivation, hand a PackageInfo object describing
 * it to `use`, which must not keep it: unlike the one returned above, it
 * keeps no unforce job's memory past the call. Returns whether it did.
 */
bool getDerivation(EvalState & state, Value & v, bool ignoreAssertionFailures, fun<void(PackageInfo &)> use);

/**
 * With `use`, each derivation found is handed to it in the unforce job that
 * found it, which `use` must not hold, and listed by name only.
 */
void getDerivations(
    EvalState & state,
    Value & v,
    const std::string & pathPrefix,
    const Bindings & autoArgs,
    PackageInfos & drvs,
    bool ignoreAssertionFailures,
    std::function<void(PackageInfo &)> use = {});

} // namespace nix
