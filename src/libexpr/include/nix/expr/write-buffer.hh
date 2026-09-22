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
#include "nix/util/object-hash.hh"
#include "nix/util/serialise.hh"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace nix {

/**
 * The NAR of one regular, non-executable file, read from bytes the caller
 * keeps: the framing `dumpString` writes, without the coroutine a
 * `sinkToSource` needs.  The flush builds one per pending object, and a
 * coroutine's stack is a mapping made and unmade for each
 * (`doc/lazy-store/05-validation.md` section 6, the wall table).  Pinned
 * against `dumpString` by `WriteBufferTest.theNarOfAStringIsDumpStrings`.
 */
struct NarOfString : Source
{
    bool isInMemory() const override
    {
        return true;
    }

    explicit NarOfString(std::shared_ptr<const std::string> contents);

    size_t read(char * data, size_t len) override;

    /** The NAR's length: the header, the bytes, their padding and the tag. */
    size_t size() const
    {
        return head.size() + contents->size() + tail.size();
    }

private:
    std::shared_ptr<const std::string> contents;
    /** Everything before the bytes, ending in their length. */
    std::string head;
    /** Their padding, then the closing tag. */
    std::string tail;
    size_t pos = 0;
};

class WriteBuffer
{
public:
    /**
     * What one pending object costs besides its own bytes, at the peak of
     * its life: the map entry while it waits (the node, the store path's
     * base name, the shared text's header: 418 B measured) and, while the
     * flush hands it over, its `ValidPathInfo` and its source as well
     * (660 B more).  An upper bound over both allocators, pinned by
     * `WriteBufferTest.aPendingObjectCostsTheBoundBesidesItsText` against
     * the allocator's own accounting (`doc/lazy-store/05-validation.md`
     * section 6).
     */
    static constexpr uint64_t perObjectOverhead = 1536;

    /**
     * What one reference of a pending object costs, likewise at the peak:
     * a set node and a base name in the entry, and again in the
     * `ValidPathInfo` the flush builds (112 B each, measured).
     */
    static constexpr uint64_t perReferenceOverhead = 256;

    /**
     * The bound on `footprint()`: reaching it writes what is pending, which
     * bounds peak memory without letting a name escape (C2), since nothing
     * has been emitted while evaluation runs.  Bytes, not a count of
     * objects: a slim entry's footprint is its text plus
     * `perObjectOverhead`, so the text is what there is to bound
     * (doc/lazy-store/04-derivation.md, the write buffer).  Not a setting:
     * the store's contents and every write count are the same at any value,
     * and past this one the peak no longer falls (05-validation.md section 6).
     */
    static constexpr uint64_t defaultFlushThreshold = 32 * 1024 * 1024;

    explicit WriteBuffer(Store & store, uint64_t flushThreshold = defaultFlushThreshold);

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

    /**
     * What the pending objects cost in memory, in bytes: their texts, their
     * references and the constants above.  An upper bound on the heap the
     * buffer holds, and the quantity the flush threshold bounds.
     */
    uint64_t footprint() const
    {
        return accounted;
    }

    bool contains(const StorePath & path) const
    {
        return pending.contains(path);
    }

    /**
     * The bound, in bytes of `footprint()`.  The tests alone set it: the
     * eager side of the builtin sweep writes every object as it is created.
     */
    void setFlushThreshold(uint64_t bytes)
    {
        flushThreshold = bytes;
    }

    /**
     * Enqueue a text object: a flat file, content-addressed over its text
     * and references, exactly what `Store::addToStoreFromDump` with the
     * `Text` method registers.  Returns its store path.  Idempotent in the
     * path. A rooted, already-valid object needs no pending write unless
     * repair was requested. The caller must realise any mounted references
     * before enqueueing, since reaching the memory bound can flush here.
     *
     * The text is a view, not a string to take over: what the buffer keeps
     * is the bytes and not the buffer they were built in.  `unparse`
     * reserves 64 KiB for every derivation it writes
     * (`src/libstore/derivation/aterm.cc:798`); an entry that took that
     * string over would hold the reservation for as long as the object was
     * pending, and its NAR-hash thunk -- which the store's path-info cache
     * keeps -- for as long as the process ran.  Measured, 4000 derivations
     * through `nix-instantiate`, which holds one at a time: 136 MB peak
     * resident with the reservation kept against 70 MB without
     * (`doc/lazy-store/05-validation.md` section 6).
     */
    StorePath
    addText(std::string_view name, std::string_view contents, StorePathSet references, RepairFlag repair = NoRepair);

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
    /**
     * What is kept for one object between its creation and its write: its
     * bytes, its references, and the text hash computed to name it. The
     * blob hash is needed only when a missing object is flushed. The
     * `ValidPathInfo` an eager write registers is built from these at the
     * flush, by the call that named the path, so that the description and
     * the name are one equation (`04-derivation.md` 1.1).
     *
     * The text is shared, not copied: the flush hands the same bytes to the
     * source it streams from and to the NAR-hash thunk an old peer may
     * force, and neither may outlive them.
     */
    struct Pending
    {
        std::shared_ptr<const std::string> contents;
        StorePathSet references;
        /** `hashString(SHA256, *contents)`: the text content address. */
        Hash textHash;
        bool repair;
        /** Scratch for the flush's reference-order walk. */
        bool emitted;
    };

    using Entry = std::pair<const StorePath, Pending>;

    /** The description an eager write registers for a pending object. */
    ValidPathInfo infoFor(const Entry & entry) const;

    Store & store;

    std::map<StorePath, Pending> pending;

    /** `footprint()`. */
    uint64_t accounted = 0;

    /**
     * Already-valid references of pending objects that this process holds a
     * temporary root on, so that they survive until the flush registers a
     * referrer for them.
     */
    StorePathSet rooted;

    /** Cap on `accounted` before the buffer flushes itself. */
    uint64_t flushThreshold;
};

} // namespace nix
