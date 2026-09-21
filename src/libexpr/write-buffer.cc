#include "nix/expr/write-buffer.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/store/derivation/aterm.hh"
#include "nix/store/globals.hh"
#include "nix/util/archive.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/serialise.hh"

#include <cassert>
#include <cstring>
#include <functional>

namespace nix {

namespace {
/* A NAR field: its length, its bytes, and zeroes to the next multiple of
   eight -- `writeString` (serialise.cc), which is what `dumpString` uses. */
void narField(std::string & to, std::string_view field)
{
    uint64_t n = field.size();
    char length[8];
    for (int i = 0; i < 8; i++)
        length[i] = (char) ((n >> (8 * i)) & 0xff);
    to.append(length, 8);
    to += field;
    if (auto over = to.size() % 8)
        to.append(8 - over, '\0');
}
} // namespace

NarOfString::NarOfString(std::shared_ptr<const std::string> contents)
    : contents(std::move(contents))
{
    /* `dumpString`: the magic, the node's opening, its type, and the
       contents field -- whose length is written here and whose bytes the
       reader takes from the string itself. */
    narField(head, narVersionMagic1);
    narField(head, "(");
    narField(head, "type");
    narField(head, "regular");
    narField(head, "contents");
    uint64_t n = this->contents->size();
    char length[8];
    for (int i = 0; i < 8; i++)
        length[i] = (char) ((n >> (8 * i)) & 0xff);
    head.append(length, 8);
    /* The contents' own padding, measured from where they end.  The
       closing tag is then a field of its own -- built in its own string,
       because `narField` pads to a multiple of eight counting from the
       string's start and the tail's start is not the stream's. */
    if (auto over = this->contents->size() % 8)
        tail.append(8 - over, '\0');
    std::string tag;
    narField(tag, ")");
    tail += tag;
}

size_t NarOfString::read(char * data, size_t len)
{
    if (pos >= size())
        throw EndOfFile("the NAR has been read");
    size_t got = 0;
    while (len && pos < size()) {
        std::string_view segment;
        size_t offset;
        if (pos < head.size()) {
            segment = head;
            offset = pos;
        } else if (pos < head.size() + contents->size()) {
            segment = *contents;
            offset = pos - head.size();
        } else {
            segment = tail;
            offset = pos - head.size() - contents->size();
        }
        auto n = std::min(len, segment.size() - offset);
        memcpy(data + got, segment.data() + offset, n);
        got += n;
        pos += n;
        len -= n;
    }
    return got;
}

WriteBuffer::WriteBuffer(Store & store, uint64_t flushThreshold)
    : store(store)
    , flushThreshold(flushThreshold)
{
}

ValidPathInfo WriteBuffer::infoFor(const Entry & entry) const
{
    auto & [path, p] = entry;
    /* The path info an eager write registers: text-addressed over the text
       and references -- the same call that named the path in `addText`, so
       that the description and the name are one equation and `info.path` is
       `path` by construction; the NAR is that of a single regular file, so
       its size is arithmetic and a plain file's object hash is its blob id;
       the NAR hash is computed only for a peer that must be sent one, from
       the same bytes the source streams; not "ultimate", which only built
       outputs are. */
    auto info = ValidPathInfo::makeFromCA(
        store,
        path.name(),
        TextInfo{
            .hash = p.textHash,
            .references = p.references,
        },
        ObjectHash{.hash = merkle::blobId(*p.contents)});
    assert(info.path == path);
    info.narSize = merkle::nar::root(merkle::nar::regular(p.contents->size(), false));
    info.lazyNarHash = [contents = p.contents] {
        HashSink narSink(HashAlgorithm::SHA256);
        dumpString(*contents, narSink);
        return narSink.finish().hash;
    };
    info.ultimate = false;
    return info;
}

StorePath
WriteBuffer::addText(std::string_view name, std::string_view contents, StorePathSet references, RepairFlag repair)
{
    auto textHash = hashString(HashAlgorithm::SHA256, contents);
    ContentAddressWithReferences address = TextInfo{
        .hash = textHash,
        .references = std::move(references),
    };
    auto path = store.makeFixedOutputPathFromCA(name, address);
    references = std::move(std::get<TextInfo>(address.raw).references);

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

    accounted += contents.size() + perObjectOverhead + references.size() * perReferenceOverhead;

    pending.emplace(
        path,
        Pending{
            .contents = std::make_shared<const std::string>(contents),
            .references = std::move(references),
            .textHash = textHash,
            .repair = repair == Repair,
            .emitted = false,
        });

    /* Bound peak memory: once the pending objects cost enough, write them
       now.  Nothing has been emitted (evaluation is still running; the output
       sinks have not yet reached their realise), so this only moves the write
       earlier and never lets a name escape before its object (C2). */
    if (accounted >= flushThreshold)
        flush();
    return path;
}

StorePath WriteBuffer::addDerivation(const Derivation & drv, RepairFlag repair)
{
    StorePathSet references;
    for (const auto & input : drv.inputs)
        references.insert(input.getBaseStorePath());
    /* Note that the outputs of a derivation are *not* references. */
    auto text = unparse(drv, store);
    return addText(std::string(drv.name) + drvExtension, text, std::move(references), repair);
}

StorePathSet WriteBuffer::pendingReferences() const
{
    StorePathSet refs;
    for (auto & [path, p] : pending)
        for (auto & ref : p.references)
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
       does one path at a time.  The query's argument is the only copy of the
       pending paths this flush makes, and it is dropped before the batch is
       built. */
    StorePathSet valid;
    {
        StorePathSet paths;
        for (auto & [path, _] : pending)
            paths.insert(path);
        valid = store.queryValidPaths(paths);
    }

    /* References before referrers: a daemon adds the batch in the order
       sent, and a reference must be valid when its referrer is registered.
       `emit` collects, in that order, the objects that must be written: one
       already valid and not being repaired is skipped, exactly as an eager
       `addToStore` skips it.  The order holds pointers into `pending`, whose
       nodes do not move, and the visited mark is a field of the entry: at
       this size a second set of store paths is a copy of the buffer.  */
    std::vector<Entry *> order;
    bool anyRepair = false;
    /* The mark is a field of the entry, so it is stale if a previous flush
       threw after marking: a failed write leaves the objects pending and the
       next flush -- the destructor's, or the next door's -- must write them. */
    for (auto & [_, p] : pending)
        p.emitted = false;
    /* A `std::function`, not a recursive `this auto & self` lambda: the
       latter ICEs GCC 15 here. */
    std::function<void(Entry &)> emit = [&](Entry & entry) -> void {
        auto & [path, p] = entry;
        if (p.emitted)
            return;
        p.emitted = true;
        /* A valid object needs no write, so neither its blob hash nor its
           reference order is needed. The outer loop still visits every
           pending object, including references of this one (C1). */
        if (valid.contains(path) && !p.repair)
            return;
        for (auto & ref : p.references)
            if (ref != path)
                if (auto it = pending.find(ref); it != pending.end())
                    emit(*it);
        anyRepair = anyRepair || p.repair;
        order.push_back(&entry);
    };
    for (auto & entry : pending)
        emit(entry);

    if (order.empty()) {
        pending.clear();
        accounted = 0;
        return;
    }

    /* The batch borrows the buffer's bytes: each source streams the very
       string the entry holds, shared with the NAR-hash thunk the info
       carries, so building the batch copies no text.  `pending` outlives the
       call. */
    Store::PathsSource batch;
    batch.reserve(order.size());
    for (auto entry : order)
        batch.emplace_back(infoFor(*entry), std::make_unique<NarOfString>(entry->second.contents));

    Activity act(*logger, lvlTalkative, actCopyPaths, fmt("writing %d store objects", order.size()));
    if (!anyRepair && !settings.getWorkerSettings().keepGoing) {
        /* The common case: nothing to repair, so one bulk add.  Every path
           in `order` is not yet valid, so the batch writes them all. */
        store.addMultipleToStore(std::move(batch), act, NoRepair, NoCheckSigs);
    } else {
        /* The bulk operation skips already-valid paths even under repair,
           and suppresses write errors under keep-going. A flush must write
           every object before its paths can escape, and must propagate a
           failed write. Use the eager operation in these cases, in reference
           order and with each object's own repair flag. */
        for (size_t i = 0; i < batch.size(); ++i) {
            /* The source is destroyed with each object, as it is in the bulk
               add (store-api.cc): a `SinkToSource` holds a coroutine stack
               from its first read until it is destroyed. */
            auto source = std::move(batch[i].second);
            store.addToStore(batch[i].first, *source, order[i]->second.repair ? Repair : NoRepair, NoCheckSigs);
        }
    }

    pending.clear();
    accounted = 0;
}

} // namespace nix
