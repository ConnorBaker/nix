#include "nix/expr/write-buffer.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/store/derivation/aterm.hh"
#include "nix/util/archive.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/serialise.hh"

#include <functional>

namespace nix {

WriteBuffer::WriteBuffer(Store & store, uint64_t flushThreshold)
    : store(store)
    , flushThreshold(flushThreshold)
{
}

StorePath WriteBuffer::addText(std::string_view name, std::string contents, StorePathSet references, RepairFlag repair)
{
    /* The path info an eager write registers: text-addressed over the text
       and references; the NAR is that of a single regular file, so its size
       is arithmetic and a plain file's object hash is its blob id; the NAR
       hash is computed only for a peer that must be sent one (a derivation
       is small: the thunk keeps its own copy of the text); not "ultimate",
       which only built outputs are. */
    auto info = ValidPathInfo::makeFromCA(
        store,
        name,
        TextInfo{
            .hash = hashString(HashAlgorithm::SHA256, contents),
            .references = references,
        },
        ObjectHash{.hash = merkle::blobId(contents)});
    info.narSize = merkle::nar::root(merkle::nar::regular(contents.size(), false));
    info.lazyNarHash = [contents] {
        HashSink narSink(HashAlgorithm::SHA256);
        dumpString(contents, narSink);
        return narSink.finish().hash;
    };
    info.ultimate = false;
    auto path = info.path;

    if (auto it = pending.find(path); it != pending.end()) {
        if (repair)
            it->second.repair = true;
        return path;
    }

    /* As the eager write does, hold a temporary root on the path from now
       on, in case it already exists. */
    store.addTempRoot(path);

    /* Its references that are not themselves pending must survive until the
       flush registers this object as their referrer. */
    for (auto & ref : references)
        if (!pending.contains(ref) && rooted.insert(ref).second)
            store.addTempRoot(ref);

    pending.emplace(
        path,
        Pending{
            .info = std::move(info),
            .contents = std::move(contents),
            .repair = repair == Repair,
        });

    /* Bound peak memory: once enough objects are pending, write them now.
       Nothing has been emitted (evaluation is still running; the output sinks
       have not yet reached their realise), so this only moves the write
       earlier and never lets a name escape before its object (C2). */
    if (pending.size() >= flushThreshold)
        flush();
    return path;
}

StorePath WriteBuffer::addDerivation(const Derivation & drv, RepairFlag repair)
{
    StorePathSet references;
    for (const auto & input : drv.inputs)
        references.insert(input.getBaseStorePath());
    /* Note that the outputs of a derivation are *not* references. */
    return addText(std::string(drv.name) + drvExtension, unparse(drv, store), std::move(references), repair);
}

StorePathSet WriteBuffer::pendingReferences() const
{
    StorePathSet refs;
    for (auto & [path, p] : pending)
        for (auto & ref : p.info.references)
            if (ref != path)
                refs.insert(ref);
    return refs;
}

void WriteBuffer::flush()
{
    if (pending.empty())
        return;

    /* One query decides which objects are already in the store; those are
       not sent again, unless repair was asked, which is what the eager write
       does one path at a time. */
    StorePathSet paths;
    for (auto & [path, _] : pending)
        paths.insert(path);
    auto valid = store.queryValidPaths(paths);

    /* References before referrers: a daemon adds the batch in the order
       sent, and a reference must be valid when its referrer is registered.
       `emit` collects, in that order, the paths that must be written: a path
       already valid and not being repaired is skipped, exactly as an eager
       `addToStore` skips it. */
    std::vector<StorePath> order;
    bool anyRepair = false;
    StorePathSet emitted;
    /* A `std::function`, not a recursive `this auto & self` lambda: the
       latter ICEs GCC 15 here. */
    std::function<void(const StorePath &)> emit = [&](const StorePath & path) -> void {
        if (!emitted.insert(path).second)
            return;
        auto & p = pending.at(path);
        for (auto & ref : p.info.references)
            if (ref != path && pending.contains(ref))
                emit(ref);
        if (valid.contains(path) && !p.repair)
            return;
        anyRepair = anyRepair || p.repair;
        order.push_back(path);
    };
    for (auto & [path, _] : pending)
        emit(path);

    auto sourceFor = [](const Pending & p) {
        return sinkToSource([contents = p.contents](Sink & sink) { dumpString(contents, sink); });
    };

    if (order.empty()) {
        pending.clear();
        return;
    }

    Activity act(*logger, lvlTalkative, actCopyPaths, fmt("writing %d store objects", order.size()));
    if (!anyRepair) {
        /* The common case: nothing to repair, so one bulk add.  Every path
           in `order` is not yet valid, so the batch writes them all. */
        Store::PathsSource batch;
        for (auto & path : order) {
            auto & p = pending.at(path);
            batch.emplace_back(p.info, sourceFor(p));
        }
        store.addMultipleToStore(std::move(batch), act, NoRepair, NoCheckSigs);
    } else {
        /* Repair was asked for at least one object.  `addMultipleToStore`
           skips every already-valid path regardless of the repair flag
           (store-api.cc), so it cannot rewrite a corrupt-but-valid object;
           mirror eager mode instead with one `addToStore` per object, in
           reference order, each carrying its own repair flag. */
        for (auto & path : order) {
            auto & p = pending.at(path);
            auto source = sourceFor(p);
            store.addToStore(p.info, *source, p.repair ? Repair : NoRepair, NoCheckSigs);
        }
    }

    pending.clear();
}

} // namespace nix
