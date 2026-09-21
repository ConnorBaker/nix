#pragma once
///@file

#include "nix/fetchers/fetchers.hh"
#include "nix/store/path.hh"

#include <utility>
#include <vector>

namespace nix::fetchers {

/**
 * A cache for arbitrary `Attrs` -> `Attrs` mappings with a timestamp
 * for expiration.
 */
struct Cache
{
private:
    /* VTable anchor to avoid weak linkage of the vtable - it breaks
       dynamic_cast across shared libraries on Darwin. */
    virtual void anchor();
public:
    virtual ~Cache() {}

    /**
     * A domain is a partition of the key/value cache for a particular
     * purpose, e.g. git revision to revcount.
     */
    using Domain = std::string_view;

    /**
     * A cache key is a domain and an arbitrary set of attributes.
     */
    using Key = std::pair<Domain, Attrs>;

    /**
     * Add a key/value pair to the cache.
     */
    virtual void upsert(const Key & key, const Attrs & value) = 0;

    /**
     * The same for many pairs at once, as one commit rather than one per
     * pair.  The rows a walk writes are a memo (`01` section 9.9): each is
     * worth only the work a later walk would repeat without it, so a batch
     * that is lost -- by a crash before it is handed over, or by the
     * rollback of a batch a later pair in it fails -- costs nothing but
     * that work.  What it buys is measured in `05` section 6: the memo's
     * per-row commit was 15 syscalls a file of the 38 a first entry paid.
     */
    virtual void upsert(const std::vector<std::pair<Key, Attrs>> & rows);

    /**
     * Look up a key with infinite TTL.
     */
    virtual std::optional<Attrs> lookup(const Key & key) = 0;

    /**
     * Look up a key. Return nothing if its TTL has exceeded
     * `settings.tarballTTL`.
     */
    virtual std::optional<Attrs> lookupWithTTL(const Key & key) = 0;

    struct Result
    {
        bool expired = false;
        Attrs value;
    };

    /**
     * Look up a key. Return a bool denoting whether its TTL has
     * exceeded `settings.tarballTTL`.
     */
    virtual std::optional<Result> lookupExpired(const Key & key) = 0;

    /**
     * Insert a cache entry that has a store path associated with
     * it. Such cache entries are always considered stale if the
     * associated store path is invalid.
     */
    virtual void upsert(Key key, Store & store, Attrs value, const StorePath & storePath) = 0;

    struct ResultWithStorePath : Result
    {
        StorePath storePath;
    };

    /**
     * Look up a store path in the cache. The returned store path will
     * be valid, but it may be expired.
     */
    virtual std::optional<ResultWithStorePath> lookupStorePath(Key key, Store & store) = 0;

    /**
     * Look up a store path in the cache. Return nothing if its TTL
     * has exceeded `settings.tarballTTL`.
     */
    virtual std::optional<ResultWithStorePath> lookupStorePathWithTTL(Key key, Store & store) = 0;
};

} // namespace nix::fetchers
