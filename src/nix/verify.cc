#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-open.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/signals.hh"
#include "nix/store/keys.hh"

#include <atomic>

#include "nix/util/exit.hh"

namespace nix {

struct CmdVerify : StorePathsCommand
{
    bool noContents = false;
    bool noTrust = false;
    std::vector<StoreReference> substituterUris;
    size_t sigsNeeded = 0;

    CmdVerify()
    {
        addFlag({
            .longName = "no-contents",
            .description = "Do not verify the contents of each store path.",
            .handler = {&noContents, true},
        });

        addFlag({
            .longName = "no-trust",
            .description = "Do not verify whether each store path is trusted.",
            .handler = {&noTrust, true},
        });

        addFlag({
            .longName = "substituter",
            .shortName = 's',
            .description = "Use signatures from the specified store.",
            .labels = {"store-uri"},
            .handler = {[&](std::string s) { substituterUris.push_back(StoreReference::parse(s)); }},
        });

        addFlag({
            .longName = "sigs-needed",
            .shortName = 'n',
            .description = "Require that each path is signed by at least *n* different keys.",
            .labels = {"n"},
            .handler = {&sigsNeeded},
        });
    }

    std::string description() override
    {
        return "verify the integrity of store paths";
    }

    std::string doc() override
    {
        return
#include "verify.md"
            ;
    }

    void run(ref<Store> store, StorePaths && storePaths) override
    {
        std::vector<ref<Store>> substituters;
        for (auto & s : substituterUris)
            substituters.push_back(openStore(StoreReference{s}));

        auto publicKeys = getDefaultPublicKeys();

        Activity act(*logger, actVerifyPaths);

        std::atomic<size_t> done{0};
        std::atomic<size_t> untrusted{0};
        std::atomic<size_t> corrupted{0};
        std::atomic<size_t> failed{0};
        std::atomic<size_t> active{0};

        auto update = [&]() { act.progress(done, storePaths.size(), active, failed); };

        ThreadPool pool;

        auto doPath = [&](const StorePath & storePath) {
            try {
                checkInterrupt();

                MaintainCount<std::atomic<size_t>> mcActive(active);
                update();

                auto info = store->queryPathInfo(storePath);

                // Note: info->path can be different from storePath
                // for binary cache stores when using --all (since we
                // can't enumerate names efficiently).
                Activity act2(*logger, lvlInfo, actUnknown, fmt("checking '%s'", store->printStorePath(info->path)));

                if (!noContents) {
                    if (auto mismatch = store->contentMismatch(*info)) {
                        corrupted++;
                        act2.result(resCorruptedPath, store->printStorePath(info->path));
                        printError(
                            "path '%s' was modified! expected hash '%s', got '%s'",
                            store->printStorePath(info->path),
                            mismatch->first,
                            mismatch->second);
                    }
                }

                if (!noTrust) {

                    bool good = false;

                    if (info->ultimate && !sigsNeeded)
                        good = true;

                    else {

                        /* `validSigs` counts distinct keys, not signatures:
                           one key may have signed both fingerprint versions
                           (`nix store sign` does), and `--sigs-needed` asks
                           for keys. */
                        std::set<std::string> keysGood; /* the form `checkSignatures` uses too */
                        size_t actualSigsNeeded = std::max(sigsNeeded, (size_t) 1);
                        size_t validSigs = 0;

                        /* A version-1 signature on a description that
                           asserts no NAR hash (a locally held path whose
                           row holds the object hash alone) is verified
                           against one walk of the path, made at most once
                           per path and only when such a signature by a
                           trusted key is met (`checkSignature`). */
                        std::optional<Hash> walkedNarHash;
                        UnkeyedValidPathInfo::NarHashThunk narHashOnce{[&]() -> Hash {
                            if (!walkedNarHash)
                                walkedNarHash = narHashOf(*store, info->path);
                            return *walkedNarHash;
                        }};

                        auto doSigs = [&](std::set<Signature> sigs) {
                            for (const auto & sig : sigs) {
                                if (validSigs >= ValidPathInfo::maxSigs || keysGood.count(sig.keyName))
                                    continue;
                                if (info->checkSignature(*store, publicKeys, sig, narHashOnce)) {
                                    keysGood.insert(sig.keyName);
                                    validSigs++;
                                }
                            }
                        };

                        if (info->isContentAddressed(*store))
                            validSigs = ValidPathInfo::maxSigs;

                        doSigs(info->sigs);

                        for (auto & store2 : substituters) {
                            if (validSigs >= actualSigsNeeded)
                                break;
                            try {
                                auto info2 = store2->queryPathInfo(info->path);
                                if (info2->isContentAddressed(*store))
                                    validSigs = ValidPathInfo::maxSigs;
                                doSigs(info2->sigs);
                            } catch (InvalidPath &) {
                            } catch (Error & e) {
                                logError(e.info());
                            }
                        }

                        if (validSigs >= actualSigsNeeded)
                            good = true;
                    }

                    if (!good) {
                        untrusted++;
                        act2.result(resUntrustedPath, store->printStorePath(info->path));
                        printError("path '%s' is untrusted", store->printStorePath(info->path));
                    }
                }

                done++;

            } catch (Error & e) {
                logError(e.info());
                failed++;
            }

            update();
        };

        for (auto & storePath : storePaths)
            pool.enqueue(std::bind(doPath, storePath));

        pool.process();

        throw Exit((corrupted ? 1 : 0) | (untrusted ? 2 : 0) | (failed ? 4 : 0));
    }
};

static auto rCmdVerify = registerCommand2<CmdVerify>({"store", "verify"});

} // namespace nix
