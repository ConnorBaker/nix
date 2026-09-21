#include "nix/store/export-import.hh"
#include "nix/util/serialise.hh"
#include "nix/store/store-api.hh"
#include "nix/util/archive.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/store/common-protocol.hh"
#include "nix/store/common-protocol-impl.hh"

#include <algorithm>

namespace nix {

static void exportPath(Store & store, const StorePath & path, Sink & sink)
{
    auto info = store.queryPathInfo(path);

    /* The NAR goes to the sink and, on the same pass, through an
       ObjectHashSink -- and through a HashSink only for a description that
       carries just an asserted NAR hash.  Refuse to export a path that has
       changed: this prevents filesystem corruption from spreading to other
       machines. */
    std::optional<HashSink> narHashSink;
    if (!info->objectHash && info->assertedNarHash)
        narHashSink.emplace(HashAlgorithm::SHA256);
    LambdaSink teeSink{[&](std::string_view data) {
        sink(data);
        if (narHashSink)
            (*narHashSink)(data);
    }};
    auto source = sinkToSource([&](Sink & s) { store.narFromPath(path, s); });
    TeeSource teeSource{*source, teeSink};
    auto object = objectHashOfNar(teeSource);

    if (info->objectHash) {
        auto current = ObjectHash::of(object.root);
        if (current != *info->objectHash)
            throw Error(
                "object hash of path '%s' has changed from '%s' to '%s'!",
                store.printStorePath(path),
                info->objectHash->render(),
                current.render());
    } else if (narHashSink) {
        auto current = narHashSink->finish().hash;
        if (current != *info->assertedNarHash)
            throw Error(
                "hash of path '%s' has changed from '%s' to '%s'!",
                store.printStorePath(path),
                info->assertedNarHash->to_string(HashFormat::Nix32, true),
                current.to_string(HashFormat::Nix32, true));
    }

    sink << exportMagic << store.printStorePath(path);
    CommonProto::write(store, CommonProto::WriteConn{.to = sink}, info->references);
    sink << (info->deriver ? store.printStorePath(*info->deriver) : "") << 0;
}

void exportPaths(Store & store, const StorePathSet & paths, Sink & sink)
{
    auto sorted = store.topoSortPaths(paths);

    for (auto & path : sorted | std::views::reverse) {
        sink << 1;
        exportPath(store, path, sink);
    }

    sink << 0;
}

StorePaths importPaths(Store & store, Source & source, CheckSigsFlag checkSigs)
{
    StorePaths res;
    while (true) {
        auto n = readNum<uint64_t>(source);
        if (n == 0)
            break;
        if (n != 1)
            throw Error("input doesn't look like something created by 'nix-store --export'");

        /* Extract the NAR from the source, hashing the tree it carries on
           the way: the object hash of what is imported, and its size. */
        StringSink saved;
        TeeSource tee{source, saved};
        ObjectHashSink hasher;
        parseDump(hasher, tee);
        auto object = hasher.finish();

        uint32_t magic = readInt(source);
        if (magic != exportMagic)
            throw Error("Nix archive cannot be imported; wrong format");

        auto path = store.parseStorePath(readString(source));

        // Activity act(*logger, lvlInfo, "importing path '%s'", info.path);

        auto references = CommonProto::Serialise<StorePathSet>::read(store, CommonProto::ReadConn{.from = source});
        auto deriver = readString(source);

        ValidPathInfo info{path, {store, ObjectHash::of(object.root)}};
        if (deriver != "")
            info.deriver = store.parseStorePath(deriver);
        info.references = references;
        info.narSize = saved.s.size();
        info.lazyNarHash = [&nar = saved.s] { return hashString(HashAlgorithm::SHA256, nar); };

        // Ignore optional legacy signature.
        if (readInt(source) == 1)
            readString(source);

        // Can't use underlying source, which would have been exhausted
        auto source = StringSource(saved.s);
        store.addToStore(info, source, NoRepair, checkSigs);

        res.push_back(info.path);
    }

    return res;
}

} // namespace nix
