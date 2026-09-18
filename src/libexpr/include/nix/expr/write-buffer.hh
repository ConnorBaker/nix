#pragma once
/**
 * @file
 *
 * Store objects evaluation has created but not yet written: derivations and
 * `builtins.toFile` texts, whose content addresses are known before `flush()`
 * writes them in reference order, wherever a path may leave the evaluator and
 * at the end.  The store's contents after a command are those of eager writes
 * (doc/lazy-store/01-specification.md, condition (C1); 04-derivation.md, 1.1).
 */

#include "nix/store/store-api.hh"
#include "nix/store/derivations.hh"

#include <cstdint>
#include <map>

namespace nix {

class WriteBuffer
{
public:
    /**
     * @param flushThreshold Cap on the number of pending objects: reaching it
     * flushes, which bounds peak memory without letting a name escape (C2),
     * since nothing has been emitted while evaluation runs.  A count, not a
     * byte size, because per-object overhead dominates the footprint
     * (doc/lazy-store/04-derivation.md, the write buffer).  The
     * `deferred-store-writes-max-pending` setting.
     */
    explicit WriteBuffer(Store & store, uint64_t flushThreshold);

    bool empty() const
    {
        return pending.empty();
    }

    /**
     * The number of pending objects; for the tests.  Production asks
     * `empty()` and `contains()`.
     */
    size_t size() const
    {
        return pending.size();
    }

    bool contains(const StorePath & path) const
    {
        return pending.contains(path);
    }

    /**
     * The references of every pending object: what must be valid before the
     * flush registers the objects that name them.
     */
    StorePathSet pendingReferences() const;

    /**
     * Enqueue a text object: a flat file, content-addressed over its text
     * and references, exactly what `Store::addToStoreFromDump` with the
     * `Text` method registers.  Returns its store path.  Idempotent in the
     * path.
     */
    StorePath
    addText(std::string_view name, std::string contents, StorePathSet references, RepairFlag repair = NoRepair);

    /**
     * Enqueue a derivation: exactly what `Store::writeDerivation` writes,
     * at the path `computeStorePath` gives.
     */
    StorePath addDerivation(const Derivation & drv, RepairFlag repair = NoRepair);

    /**
     * Write everything pending to the store, references before referrers,
     * in one batch, and forget it.  Objects that are already valid are not
     * sent again unless repair was asked for them.  Total: there is nothing
     * to choose.
     */
    void flush();

private:
    struct Pending
    {
        ValidPathInfo info;
        std::string contents;
        bool repair;
    };

    Store & store;

    std::map<StorePath, Pending> pending;

    /**
     * Already-valid references of pending objects that this process holds a
     * temporary root on, so that they survive until the flush registers a
     * referrer for them.
     */
    StorePathSet rooted;

    /** Cap on the number of pending objects before the buffer flushes itself. */
    const uint64_t flushThreshold;
};

} // namespace nix
