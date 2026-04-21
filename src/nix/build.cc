#include "nix/cmd/command.hh"
#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/store/build-debugger.hh"
#include "nix/store/store-api.hh"
#include "nix/store/globals.hh"
#include "nix/util/experimental-features.hh"

#include <nlohmann/json.hpp>

#include <unistd.h>

namespace nix {

/* This serialization code is diferent from the canonical (single)
   derived path serialization because:

   - It looks up output paths where possible

   - It includes the store dir in store paths

   We might want to replace it with the canonical format at some point,
   but that would be a breaking change (to a still-experimental but
   widely-used command, so that isn't being done at this time just yet.
 */

static nlohmann::json toJSON(Store & store, const SingleDerivedPath::Opaque & o)
{
    return store.printStorePath(o.path);
}

static nlohmann::json toJSON(Store & store, const SingleDerivedPath & sdp);
static nlohmann::json toJSON(Store & store, const DerivedPath & dp);

static nlohmann::json toJSON(Store & store, const SingleDerivedPath::Built & sdpb)
{
    nlohmann::json res;
    res["drvPath"] = toJSON(store, *sdpb.drvPath);
    // Fallback for the input-addressed derivation case: We expect to always be
    // able to print the output paths, so let’s do it
    // FIXME try-resolve on drvPath
    const auto outputMap = store.queryPartialDerivationOutputMap(resolveDerivedPath(store, *sdpb.drvPath));
    res["output"] = sdpb.output;
    auto outputPathIter = outputMap.find(sdpb.output);
    if (outputPathIter == outputMap.end())
        res["outputPath"] = nullptr;
    else if (std::optional p = outputPathIter->second)
        res["outputPath"] = store.printStorePath(*p);
    else
        res["outputPath"] = nullptr;
    return res;
}

static nlohmann::json toJSON(Store & store, const DerivedPath::Built & dpb)
{
    nlohmann::json res;
    res["drvPath"] = toJSON(store, *dpb.drvPath);
    // Fallback for the input-addressed derivation case: We expect to always be
    // able to print the output paths, so let’s do it
    // FIXME try-resolve on drvPath
    const auto outputMap = store.queryPartialDerivationOutputMap(resolveDerivedPath(store, *dpb.drvPath));
    for (const auto & [output, outputPathOpt] : outputMap) {
        if (!dpb.outputs.contains(output))
            continue;
        if (outputPathOpt)
            res["outputs"][output] = store.printStorePath(*outputPathOpt);
        else
            res["outputs"][output] = nullptr;
    }
    return res;
}

static nlohmann::json toJSON(Store & store, const SingleDerivedPath & sdp)
{
    return std::visit([&](const auto & buildable) { return toJSON(store, buildable); }, sdp.raw());
}

static nlohmann::json toJSON(Store & store, const DerivedPath & dp)
{
    return std::visit([&](const auto & buildable) { return toJSON(store, buildable); }, dp.raw());
}

static nlohmann::json derivedPathsToJSON(const DerivedPaths & paths, Store & store)
{
    auto res = nlohmann::json::array();
    for (auto & t : paths) {
        res.push_back(toJSON(store, t));
    }
    return res;
}

static nlohmann::json
builtPathsWithResultToJSON(const std::vector<BuiltPathWithResult> & buildables, const Store & store)
{
    auto res = nlohmann::json::array();
    for (auto & b : buildables) {
        auto j = b.path.toJSON(store);
        if (b.result) {
            if (b.result->startTime)
                j["startTime"] = b.result->startTime;
            if (b.result->stopTime)
                j["stopTime"] = b.result->stopTime;
            if (b.result->cpuUser)
                j["cpuUser"] = ((double) b.result->cpuUser->count()) / 1000000;
            if (b.result->cpuSystem)
                j["cpuSystem"] = ((double) b.result->cpuSystem->count()) / 1000000;
        }
        res.push_back(j);
    }
    return res;
}

struct CmdBuild : InstallablesCommand, MixOutLinkByDefault, MixDryRun, MixJSON, MixProfile
{
    bool printOutputPaths = false;
    BuildMode buildMode = bmNormal;

    CmdBuild()
    {
        addFlag({
            .longName = "print-out-paths",
            .description = "Print the resulting output paths",
            .handler = {&printOutputPaths, true},
        });

        addFlag({
            .longName = "rebuild",
            .description = "Rebuild an already built package and compare the result to the existing store paths.",
            .handler = {&buildMode, bmCheck},
        });

        // `--build-debugger` is wired only on `nix build` because only this
        // subcommand resolves the installable and populates
        // `build-debugger-target`. `MixCommonArgs` unregisters the auto-
        // generated flag for every subcommand; this re-adds it, with a
        // richer handler that validates the client-side gates eagerly.
        addFlag({
            .longName = "build-debugger",
            .description = R"(
On a failing build of the **specific** installable named on this command,
pause the failed sandbox and print instructions for attaching an
interactive `bash` shell inside it via a separate `nix debug-attach`
invocation.

Analogous in spirit to `breakpointHook` in nixpkgs, with the key
difference that no derivation modification is required. The paused
shell inherits the builder's post-failure environment — `$out`,
`$NIX_BUILD_TOP`, stdenv phase variables, partial build artifacts —
so you can inspect what went wrong and re-run failing commands manually.

Workflow:

1. Run `nix build --build-debugger <installable>`. When the build
   fails, the wrapper prints an attach command and blocks the sandbox
   for up to an hour so you have time to attach.
2. In another terminal on the same host, run the printed
   `sudo nix debug-attach <drv>` command. This enters the sandbox's
   Linux namespaces via `nsenter` and gives you an interactive shell.
3. When you exit the shell, the paused build is signaled to terminate
   and the original `nix build` reports the build as failed with the
   original exit code. The sandbox directory is preserved (as with
   [`--keep-failed`](#opt-keep-failed)) for further inspection.

Requirements:
* Linux only (depends on Linux namespaces + `nsenter`).
* Requires the experimental feature `build-debugger`.
* Daemon-mode clients must be in `trusted-users` (the pause blocks a
  daemon worker for up to an hour; untrusted users could otherwise DoS
  the daemon by stacking paused sandboxes).
* Only works when the derivation's builder is `bash` (standard
  stdenv-style). External-builder derivations are refused. CA
  (content-addressed) and fixed-output derivations are refused at
  CLI parse time (they are resolved before the build and the target
  identity doesn't match).
* The `nix debug-attach` invocation must run on the same host as the
  build; `nsenter` needs access to the sandbox's `/proc` entries.
  For remote daemons or remote builders, SSH to that host first.

The hook is scoped to exactly the one installable passed alongside
`--build-debugger`; dependencies in the closure build normally without
instrumentation, so `--max-jobs > 1` and `--keep-going` are safe and
unrestricted.
            )",
            .category = miscCategory,
            .handler = {[&]() {
                experimentalFeatureSettings.require(Xp::BuildDebugger);
#ifndef __linux__
                throw UsageError(
                    "`--build-debugger` is Linux-only (depends on Linux "
                    "namespaces + `nsenter` to enter the failed build sandbox)");
#else
                settings.buildDebugger.override(true);
#endif
            }},
            .experimentalFeature = Xp::BuildDebugger,
        });

        // The auto-generated `--no-<setting>` for every `Setting<bool>`
        // is erased globally in `MixCommonArgs`; re-add the negative
        // form here so `nix build --no-build-debugger` can override a
        // `build-debugger = true` in `nix.conf` on a per-invocation
        // basis without having to touch config. No experimental-feature
        // gate on this one — disabling a feature shouldn't require
        // opting into the feature.
        addFlag({
            .longName = "no-build-debugger",
            .description =
                "Disable [`--build-debugger`](#opt-build-debugger) for this "
                "invocation. Useful when `build-debugger = true` is set in "
                "`nix.conf` and you want to build a single target without "
                "pausing on failure.",
            .category = miscCategory,
            .handler = {[&]() { settings.buildDebugger.override(false); }},
        });
    }

    std::string description() override
    {
        return "build a derivation or fetch a store path";
    }

    std::string doc() override
    {
        return
#include "build.md"
            ;
    }

    void run(ref<Store> store, Installables && installables) override
    {
        if (settings.buildDebugger) {
            // `--build-debugger` + `--repair`: the repair path rebuilds a
            // derivation whose outputs already exist and compares the
            // result against the on-disk version. Instrumenting that
            // with the debug wrapper doesn't buy the user anything useful
            // (the interesting signal is the hash mismatch, not a paused
            // sandbox), and the semantics of "user types `exit 0`" in the
            // debug shell become particularly confusing during a repair.
            //
            // `--build-debugger` + `--rebuild` (i.e. bmCheck): same family
            // of confusion — the build only fails if the outputs disagree,
            // not if the builder exited non-zero, and the debug wrapper
            // only fires on non-zero builder exit. Catching this at
            // parse time keeps the surface small.
            if (repair)
                throw UsageError(
                    "`--build-debugger` is incompatible with `--repair` — the "
                    "repair mode's contract is about output-hash agreement, "
                    "not builder exit status; the debug wrapper only fires on "
                    "a non-zero builder exit and would add no useful signal.");
            if (buildMode == bmCheck)
                throw UsageError(
                    "`--build-debugger` is incompatible with `--rebuild` "
                    "(`--check`) — the check mode's contract is about output-"
                    "hash agreement, not builder exit status; the debug "
                    "wrapper only fires on a non-zero builder exit and would "
                    "add no useful signal.");

            // `--build-debugger` only makes sense for exactly one installable
            // and only with a command that actually builds something. Resolve
            // the installable to a concrete drv path now and stash it in
            // `build-debugger-target`; `DerivationBuilderImpl::startBuild`
            // checks this against its own `drvPath` and only applies the
            // hook when they match — dependencies in the closure build
            // normally.
            if (installables.size() != 1)
                throw UsageError(
                    "`--build-debugger` requires exactly one installable to target; got %d",
                    installables.size());

            auto derivedPaths = installables.front()->toDerivedPaths();
            if (derivedPaths.size() != 1)
                throw UsageError(
                    "`--build-debugger`: the installable resolves to %d derived paths; "
                    "the target must be a single derivation.",
                    derivedPaths.size());

            auto & path = derivedPaths.front().path;
            auto * built = std::get_if<DerivedPath::Built>(&path.raw());
            if (!built)
                throw UsageError(
                    "`--build-debugger`: the installable is not a buildable derivation "
                    "(it resolves to an opaque store path — nothing to debug).");

            auto drvPath = built->drvPath->getBaseStorePath();

            // Load the drv to check for CA / FOD. Content-addressed drvs
            // are resolved at build time and the resolved drv's path
            // differs from what the user targeted — the debugger's
            // per-goal `shouldApplyBuildDebugger` check compares by
            // drvPath and would silently miss the resolved goal. Refuse
            // at CLI parse time with a clear error. Fixed-output
            // derivations also return `isCA()==true` but they don't use
            // a bash builder anyway, so they'd be refused later by the
            // builder-is-bash preflight — catching them here just
            // produces a more informative error earlier.
            auto drv = store->readDerivation(drvPath);
            if (drv.type().isCA())
                throw UsageError(
                    "`--build-debugger` does not currently support "
                    "content-addressed derivations (including fixed-output "
                    "derivations). `%s` is content-addressed. CA resolution "
                    "renames the derivation at build time, and the debugger's "
                    "target-path comparison doesn't follow that rename. "
                    "Workaround: remove `__contentAddressed = true` while "
                    "debugging, or invoke `--build-debugger` on the already-"
                    "resolved derivation path.",
                    store->printStorePath(drvPath));

            settings.buildDebuggerTarget.override(store->printStorePath(drvPath));

            // If we're building against a remote daemon (ssh-ng://host
            // or ssh://host), the paused sandbox will live on the
            // remote host. Write a local redirect attach-info so that
            // `nix debug-attach <drv>` on this host auto-SSHes to the
            // right place. The remote daemon still instruments the
            // build via SetOptions propagation of `buildDebugger` /
            // `buildDebuggerTarget`; auto-redirect on the dispatch side
            // means the user doesn't need to pass `--on <host>`.
            auto ref = store->config.getReference();
            if (auto * spec = std::get_if<StoreReference::Specified>(&ref.variant);
                spec && (spec->scheme == "ssh" || spec->scheme == "ssh-ng"))
            {
                auto printed = store->printStorePath(drvPath);
                auto remoteHost = spec->scheme + "://" + spec->authority;
                (void) writeDebuggerRedirectAttachInfo(*store, drvPath, remoteHost);
                printInfo(
                    "build-debugger: `%s` is running on remote store `%s`. "
                    "To attach when it fails, run `sudo nix debug-attach %s` "
                    "on this host (it will SSH to the remote).",
                    printed, remoteHost, printed);
            }
        }

        if (dryRun) {
            std::vector<DerivedPath> pathsToBuild;

            for (auto & i : installables)
                for (auto & b : i->toDerivedPaths())
                    pathsToBuild.push_back(b.path);

            printMissing(store, pathsToBuild, lvlError);

            if (json)
                printJSON(derivedPathsToJSON(pathsToBuild, *store));

            return;
        }

        auto buildables =
            Installable::build(getEvalStore(), store, Realise::Outputs, installables, repair ? bmRepair : buildMode);

        if (json)
            logger->cout("%s", builtPathsWithResultToJSON(buildables, *store).dump());

        createOutLinksMaybe(buildables, store);

        if (printOutputPaths) {
            logger->stop();
            for (auto & buildable : buildables) {
                std::visit(
                    overloaded{
                        [&](const BuiltPath::Opaque & bo) { logger->cout(store->printStorePath(bo.path)); },
                        [&](const BuiltPath::Built & bfd) {
                            for (auto & output : bfd.outputs) {
                                logger->cout(store->printStorePath(output.second));
                            }
                        },
                    },
                    buildable.path.raw());
            }
        }

        BuiltPaths buildables2;
        for (auto & b : buildables)
            buildables2.push_back(b.path);
        updateProfile(*store, buildables2);
    }
};

static auto rCmdBuild = registerCommand<CmdBuild>("build");

} // namespace nix
