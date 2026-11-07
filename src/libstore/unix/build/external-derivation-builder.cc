#include "unix-derivation-builder-impl.hh"
#include "nix/util/system-error.hh"

namespace nix {

namespace {

struct ExternalDerivationBuilder : UnixDerivationBuilderImpl
{
    ExternalBuilder externalBuilder;

    ExternalDerivationBuilder(
        std::shared_ptr<BuildingStore> store,
        std::shared_ptr<DerivationBuilderCallbacks> miscMethods,
        DerivationBuilderParams params,
        ExternalBuilder externalBuilder)
        : UnixDerivationBuilderImpl(store, miscMethods, std::move(params))
        , externalBuilder(std::move(externalBuilder))
    {
        experimentalFeatureSettings.require(Xp::ExternalBuilders);
    }

    std::filesystem::path tmpDirInSandbox() override
    {
        /* In a sandbox, for determinism, always use the same temporary
           directory. */
        return "/build";
    }

    void setBuildTmpDir() override
    {
        tmpDir = topTmpDir / "build";
        createDir(tmpDir, 0700);
    }

    void startChild() override
    {
        if (drvOptions.getRequiredSystemFeatures(drv).count("recursive-nix"))
            throw Error("'recursive-nix' is not supported yet by external derivation builders");

        auto json = nlohmann::json::object();

        json.emplace("version", 1);
        json.emplace("builder", drv.builder);
        {
            auto l = nlohmann::json::array();
            for (auto & i : drv.args)
                l.push_back(rewriteStrings(i, inputRewrites));
            json.emplace("args", std::move(l));
        }
        {
            auto j = nlohmann::json::object();
            for (auto & [name, value] : env)
                j.emplace(name, rewriteStrings(value, inputRewrites));
            json.emplace("env", std::move(j));
        }
        json.emplace("topTmpDir", topTmpDir.native());
        json.emplace("tmpDir", tmpDir.native());
        json.emplace("tmpDirInSandbox", tmpDirInSandbox().native());
        json.emplace("storeDir", store->storeDir);
        json.emplace("realStoreDir", store->getRealStoreDir().native());
        json.emplace("system", drv.platform);
        {
            auto l = nlohmann::json::array();
            for (auto & i : inputPaths)
                l.push_back(store->printStorePath(i));
            json.emplace("inputPaths", std::move(l));
        }
        {
            auto l = nlohmann::json::object();
            for (auto & i : scratchOutputs)
                l.emplace(i.first, store->printStorePath(i.second));
            json.emplace("outputs", std::move(l));
        }

        // TODO(cole-h): writing this to stdin is too much effort right now, if we want to revisit
        // that, see this comment by Eelco about how to make it not suck:
        // https://github.com/DeterminateSystems/nix-src/pull/141#discussion_r2205493257
        auto jsonFile = std::filesystem::path{topTmpDir} / "build.json";
        writeFile(jsonFile, json.dump());

        /* What `openSlave`, `commonChildInit` and `setUser` did in a forked
           child, prepared here: a fork of this process must not allocate. */
        SpawnOptions options{
            .program = externalBuilder.program,
            .lookupPath = false,
            .args = OsStrings(externalBuilder.args.begin(), externalBuilder.args.end()),
            .chdir = tmpDir,
            .setSid = true,
            .restoreMounts = false,
        };
        options.args.push_back(jsonFile.native());
        if (buildUser) {
            options.uid = buildUser->getUID();
            options.gid = buildUser->getGID();
            options.supplementaryGroups = buildUser->getSupplementaryGIDs();
        }

        chownToBuilder(topTmpDir);

        auto slave = openRawSlave();
        auto devNull = openNullFD();

        auto fds = std::to_array<FdRedirection>({
            {.from = devNull.get(), .to = STDIN_FILENO},
            {.from = slave.get(), .to = STDOUT_FILENO},
            {.from = slave.get(), .to = STDERR_FILENO},
        });

        debug("executing external builder: %s %s", PathFmt(options.program), concatStringsSep(" ", options.args));
        pid = spawnProgram(options, fds);
    }
};

} // namespace

DerivationBuilderUnique makeExternalDerivationBuilder(
    std::shared_ptr<BuildingStore> store,
    std::shared_ptr<DerivationBuilderCallbacks> miscMethods,
    DerivationBuilderParams params,
    const ExternalBuilder & handler)
{
    return DerivationBuilderUnique(new ExternalDerivationBuilder(store, miscMethods, std::move(params), handler));
}

} // namespace nix
