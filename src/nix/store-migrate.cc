#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/store/local-store.hh"
#include "nix/store/store-cast.hh"
#include "nix/util/exit.hh"
#include "nix/util/signals.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/util.hh"

#include <atomic>

namespace nix {

struct CmdStoreMigrate : StoreCommand
{
    std::string description() override
    {
        return "fill the object hash of every store path registered by an older Nix";
    }

    std::string doc() override
    {
        return
#include "store-migrate.md"
            ;
    }

    void run(ref<Store> store) override
    {
        auto & localStore = require<LocalStore>(*store);

        if (localStore.config->readOnly)
            throw UsageError("cannot migrate a store opened with 'read-only'");

        auto paths = localStore.queryAllValidPaths();

        Activity act(*logger, lvlInfo, actUnknown, "migrating store path infos");

        std::atomic<size_t> done{0};
        std::atomic<size_t> skipped{0};
        std::atomic<size_t> failed{0};
        std::atomic<size_t> active{0};

        auto update = [&]() { act.progress(done, paths.size(), active, failed); };

        ThreadPool pool;

        /* One row per path, each its own transaction, so interrupting and
           rerunning loses nothing.  The path is rooted here, as
           `optimiseStore` roots each path before entering it (the other
           writer of an older row's object hash, through the same
           `recordObjectHash`); `migratedPathInfo` itself takes no root,
           since the collector queries infos under its own lock. */
        auto doPath = [&](const StorePath & path) {
            try {
                checkInterrupt();

                MaintainCount<std::atomic<size_t>> mcActive(active);
                update();

                localStore.addTempRoot(path);
                localStore.migratePathInfo(path);

                done++;
            } catch (InvalidPath &) {
                /* Collected between the enumeration and its turn: no row to
                   migrate. */
                skipped++;
            } catch (Error & e) {
                logError(e.info());
                failed++;
            }

            update();
        };

        for (auto & path : paths)
            pool.enqueue(std::bind(doPath, path));

        pool.process();

        notice(
            "%d store paths checked, %d skipped (collected meanwhile), %d failed",
            done.load(),
            skipped.load(),
            failed.load());

        throw Exit(failed ? 1 : 0);
    }
};

static auto rCmdStoreMigrate = registerCommand2<CmdStoreMigrate>({"store", "migrate"});

} // namespace nix
