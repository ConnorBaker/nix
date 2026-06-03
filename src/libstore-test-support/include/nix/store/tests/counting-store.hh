#pragma once
///@file

#include "nix/store/store-api.hh"
#include "nix/util/callback.hh"

#include <atomic>

namespace nix {

/**
 * A store decorator that delegates everything to a backing store but counts
 * the store-write operations, for tests that need to assert *how many* writes
 * happen (LD-P6 dedup "writes once", LD-D3 round-trip cost, LD-C1
 * black-hole-before-encode). Construct over a real backing store (e.g.
 * `openStore("local?root=…")`) and pass it where an `AsyncPathWriter` or other
 * writer would write.
 */
struct CountingStore : Store
{
    ref<Store> next;

    /** Number of `addMultipleToStore` calls (bulk submissions / round-trips). */
    std::atomic<size_t> bulkWrites{0};
    /** Number of distinct store objects written across all bulk submissions. */
    std::atomic<size_t> pathsWritten{0};
    /** Number of `addToStoreFromDump` calls. */
    std::atomic<size_t> dumpWrites{0};

    CountingStore(ref<Store> next)
        : Store(next->config)
        , next(next)
    {
    }

    void anchor() override {}

    void
    addMultipleToStore(PathsSource && pathsToCopy, Activity & act, RepairFlag repair, CheckSigsFlag checkSigs) override
    {
        ++bulkWrites;
        pathsWritten += pathsToCopy.size();
        next->addMultipleToStore(std::move(pathsToCopy), act, repair, checkSigs);
    }

    StorePath addToStoreFromDump(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod,
        ContentAddressMethod hashMethod,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        RepairFlag repair) override
    {
        ++dumpWrites;
        ++pathsWritten;
        return next->addToStoreFromDump(dump, name, dumpMethod, hashMethod, hashAlgo, references, repair);
    }

    /* --- pure-virtual delegation --- */

    bool isValidPathUncached(const StorePath & path) override
    {
        return next->isValidPath(path);
    }

    void queryPathInfoUncached(
        const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override
    {
        try {
            callback(std::make_shared<ValidPathInfo>(*next->queryPathInfo(path)));
        } catch (InvalidPath &) {
            callback(nullptr);
        } catch (...) {
            callback.rethrow();
        }
    }

    void queryRealisationUncached(
        const DrvOutput & id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override
    {
        next->queryRealisation(id, std::move(callback));
    }

    std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) override
    {
        return next->queryPathFromHashPart(hashPart);
    }

    void addToStore(const ValidPathInfo & info, Source & source, RepairFlag repair, CheckSigsFlag checkSigs) override
    {
        ++pathsWritten;
        next->addToStore(info, source, repair, checkSigs);
    }

    void registerDrvOutput(const Realisation & output) override
    {
        next->registerDrvOutput(output);
    }

    void addTempRoot(const StorePath & path) override
    {
        next->addTempRoot(path);
    }

    ref<SourceAccessor> getFSAccessor(bool requireValidPath) override
    {
        return next->getFSAccessor(requireValidPath);
    }

    std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath & path, bool requireValidPath) override
    {
        return next->getFSAccessor(path, requireValidPath);
    }

    std::optional<TrustedFlag> isTrustedClient() override
    {
        return next->isTrustedClient();
    }
};

} // namespace nix
