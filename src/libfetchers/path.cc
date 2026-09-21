#include "nix/fetchers/fetchers.hh"
#include "nix/store/store-api.hh"
#include "nix/util/archive.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/store/local-store.hh"
#include "nix/util/nested-work-pool.hh"

namespace nix::fetchers {

namespace {

/* Retain exactly one observation of a mutable source. File bytes are
   bounded in memory and spill to anonymous files; names and modes
   remain in a complete immutable tree. Neither capture nor replay needs
   an intermediate NAR encoder/parser. */
struct PreparedTree : SourceAccessor
{
    void anchor() override {}

    struct Tape : Sink
    {
        size_t memoryLimit;
        std::string memory;
        AutoCloseFD file;
        uint64_t size = 0;

        explicit Tape(size_t memoryLimit)
            : memoryLimit(memoryLimit)
        {
        }

        void operator()(std::string_view bytes) override
        {
            size += bytes.size();
            if (!file && memory.size() + bytes.size() <= memoryLimit) {
                memory.append(bytes);
                return;
            }
            if (!file) {
                file = createAnonymousTempFile();
                writeFull(file.get(), memory);
                std::string().swap(memory);
            }
            writeFull(file.get(), bytes);
        }
    };

    struct Contents
    {
        Tape * tape;
        uint64_t offset;
        uint64_t size;
        Hash hash;
    };

    using Node = fso::VariantT<Contents, true>;
    static constexpr size_t memoryLimit = 64 * 1024 * 1024;
    std::mutex tapesMutex;
    std::map<std::thread::id, std::unique_ptr<Tape>> tapes;
    std::optional<Node> root;

    /* One append-only tape per participating thread. A file's bytes are
       contiguous, and no tape is written by two workers. Dividing the
       budget by the pool width keeps the total content buffer bounded. */
    Tape & tape(size_t workers)
    {
        std::lock_guard lock(tapesMutex);
        auto & tape = tapes[std::this_thread::get_id()];
        if (!tape)
            tape = std::make_unique<Tape>(memoryLimit / workers);
        return *tape;
    }

    const Node * find(const CanonPath & path) const
    {
        auto * node = &*root;
        for (auto part : path) {
            auto * dir = std::get_if<Node::Directory>(&node->raw);
            if (!dir)
                return nullptr;
            auto i = dir->entries.find(part);
            if (i == dir->entries.end())
                return nullptr;
            node = &i->second;
        }
        return node;
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto * node = find(path);
        if (!node)
            return std::nullopt;
        if (auto reg = std::get_if<Node::Regular>(&node->raw))
            return Stat{.type = tRegular, .fileSize = reg->contents.size, .isExecutable = reg->executable};
        return Stat{.type = std::holds_alternative<Node::Directory>(node->raw) ? tDirectory : tSymlink};
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto * node = find(path);
        if (!node || !std::holds_alternative<Node::Directory>(node->raw))
            throw Error("'%s' is not a directory in the captured source", path);
        DirEntries entries;
        for (auto & [name, child] : std::get<Node::Directory>(node->raw).entries)
            entries.emplace(
                name,
                std::holds_alternative<Node::Directory>(child.raw) ? tDirectory
                : std::holds_alternative<Node::Regular>(child.raw) ? tRegular
                                                                   : tSymlink);
        return entries;
    }

    std::optional<merkle::TreeEntry> getKnownEntry(const CanonPath & path) override
    {
        auto * node = find(path);
        if (node)
            if (auto reg = std::get_if<Node::Regular>(&node->raw))
                return merkle::TreeEntry{
                    .mode = reg->executable ? merkle::Mode::Executable : merkle::Mode::Regular,
                    .hash = reg->contents.hash};
        return std::nullopt;
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        auto * node = find(path);
        if (!node || !std::holds_alternative<Node::Regular>(node->raw))
            throw Error("'%s' is not a regular file in the captured source", path);
        auto & contents = std::get<Node::Regular>(node->raw).contents;
        sizeCallback(contents.size);
        if (contents.tape->file)
            copyFdRange(contents.tape->file.get(), contents.offset, contents.size, sink, /*tryCoW=*/true);
        else
            sink(std::string_view(contents.tape->memory).substr(contents.offset, contents.size));
    }

    std::string readLink(const CanonPath & path) override
    {
        auto * node = find(path);
        if (!node || !std::holds_alternative<Node::Symlink>(node->raw))
            throw Error("'%s' is not a symlink in the captured source", path);
        return std::get<Node::Symlink>(node->raw).target;
    }

    ObjectHashSink::Result capture(SourceAccessor & source)
    {
        struct Captured
        {
            HashedNode hash;
            Node node;
        };

        struct Capture : NodeVisitor<Captured>
        {
            PreparedTree & bytes;
            size_t workers;
            std::unique_ptr<NestedWorkPool> pool;

            explicit Capture(PreparedTree & bytes)
                : bytes(bytes)
            {
                workers = LocalStore::copyWorkersForTest.load(std::memory_order_relaxed);
                if (!workers)
                    workers = std::min<size_t>(std::max<unsigned>(std::thread::hardware_concurrency(), 1), 8);
            }

            Captured regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) override
            {
                struct File : CreateRegularFileSink
                {
                    Tape & bytes;
                    CreateRegularFileSink & hashing;

                    File(Tape & bytes, CreateRegularFileSink & hashing)
                        : bytes(bytes)
                        , hashing(hashing)
                    {
                    }

                    void isExecutable() override
                    {
                        hashing.isExecutable();
                    }

                    void preallocateContents(uint64_t size) override
                    {
                        hashing.preallocateContents(size);
                    }

                    void operator()(std::string_view data) override
                    {
                        hashing(data);
                        bytes(data);
                    }
                };

                auto & tape = bytes.tape(workers);
                auto offset = tape.size;
                auto result = HashingVisitor{}.regular(path, [&](CreateRegularFileSink & hashing) {
                    File file{tape, hashing};
                    read(file);
                });
                Node node = Node::Regular{
                    .executable = result.entry.mode == merkle::Mode::Executable,
                    .contents = {&tape, offset, tape.size - offset, result.entry.hash}};
                return {std::move(result), std::move(node)};
            }

            Captured symlink(const CanonPath & path, const std::string & target) override
            {
                return {HashingVisitor{}.symlink(path, target), Node::Symlink{target}};
            }

            Captured directory(const CanonPath & path, Children children) override
            {
                struct Child
                {
                    std::string name;
                    bool directory;
                    fun<Captured()> visit;
                    std::optional<Captured> result;
                    std::future<void> running;
                };

                std::vector<Child> kids;
                children([&](std::string_view name, bool directory, fun<Captured()> visit) {
                    kids.push_back({std::string(name), directory, std::move(visit), {}, {}});
                });
                Finally join{[&] {
                    for (auto & child : kids)
                        if (child.running.valid())
                            pool->wait(child.running, &kids);
                }};
                bool fork = workers > 1 && kids.size() > 1;
                if (fork && !pool && std::any_of(kids.begin(), kids.end(), [](auto & c) { return c.directory; }))
                    pool = std::make_unique<NestedWorkPool>(workers);
                for (auto & child : kids) {
                    if (fork && child.directory)
                        child.running = pool->tryAsync(&kids, [&child] { child.result = child.visit(); });
                    if (!child.running.valid())
                        child.result = child.visit();
                }
                for (auto & child : kids)
                    if (child.running.valid()) {
                        pool->wait(child.running, &kids);
                        child.running.get();
                    }
                Node::Directory dir;
                auto result = HashingVisitor{}.directory(path, [&](auto each) {
                    for (auto & child : kids) {
                        dir.entries.emplace(child.name, std::move(child.result->node));
                        each(child.name, child.directory, [&] { return std::move(child.result->hash); });
                    }
                });
                return {std::move(result), std::move(dir)};
            }
        } visitor{*this};

        auto result = traverse(source, CanonPath::root, defaultPathFilter, visitor);
        root = std::move(result.node);
        return ObjectHashSink::Result::of(result.hash);
    }
};

} // namespace

struct PathInputScheme : InputScheme
{
    std::optional<Input> inputFromURL(const ParsedURL & url, bool requireTree) const override
    {
        if (url.scheme != "path")
            return {};

        if (url.authority && url.authority->host.size())
            throw Error("path URL '%s' should not have an authority ('%s')", url, *url.authority);

        Input input{};
        input.attrs.insert_or_assign("type", "path");
        input.attrs.insert_or_assign("path", urlPathToPath(url.path).string());

        for (auto & [name, value] : url.query)
            if (name == "rev" || name == "narHash" || name == "treeHash")
                input.attrs.insert_or_assign(name, value);
            else if (name == "revCount" || name == "lastModified") {
                if (auto n = string2Int<uint64_t>(value))
                    input.attrs.insert_or_assign(name, *n);
                else
                    throw Error("path URL '%s' has invalid parameter '%s'", url, name);
            } else
                throw Error("path URL '%s' has unsupported parameter '%s'", url, name);

        return input;
    }

    std::string_view schemeName() const override
    {
        return "path";
    }

    std::string schemeDescription() const override
    {
        // TODO
        return "";
    }

    const std::map<std::string, AttributeInfo> & schemeAttrs() const override
    {
        static const std::map<std::string, AttributeInfo> attrs = {
            {
                "path",
                {},
            },
            /* Allow the user to pass in "fake" tree info
               attributes. This is useful for making a pinned tree work
               the same as the repository from which is exported (e.g.
               path:/nix/store/...-source?lastModified=1585388205&rev=b0c285...).
             */
            {
                "rev",
                {},
            },
            {
                "revCount",
                {},
            },
            {
                "lastModified",
                {},
            },
        };
        return attrs;
    }

    std::optional<Input> inputFromAttrs(const Attrs & attrs) const override
    {
        getStrAttr(attrs, "path");

        Input input{};
        input.attrs = attrs;
        return input;
    }

    ParsedURL toURL(const Input & input) const override
    {
        auto query = attrsToQuery(input.attrs);
        query.erase("path");
        query.erase("type");
        query.erase("__final");
        return ParsedURL{
            .scheme = "path",
            .path = pathToUrlPath(std::filesystem::path{getStrAttr(input.attrs, "path")}),
            .query = query,
        };
    }

    std::optional<std::filesystem::path> getSourcePath(const Input & input) const override
    {
        return getAbsPath(input);
    }

    void putFile(
        const Input & input,
        const CanonPath & path,
        std::string_view contents,
        std::optional<std::string> commitMsg) const override
    {
        writeFile(getAbsPath(input) / path.rel(), contents);
    }

    std::optional<std::filesystem::path> isRelative(const Input & input) const override
    {
        std::filesystem::path path = getStrAttr(input.attrs, "path");
        if (path.is_absolute())
            return std::nullopt;
        else
            return path;
    }

    bool isLocked(const Settings & settings, const Input & input) const override
    {
        return input.getTreeHash() || input.getNarHash();
    }

    std::filesystem::path getAbsPath(const Input & input) const
    {
        std::filesystem::path path = getStrAttr(input.attrs, "path");

        if (path.is_absolute())
            return canonPath(path);

        throw Error("cannot fetch input '%s' because it uses a relative path", input.to_string());
    }

    std::pair<ref<SourceAccessor>, Input>
    getAccessor(const Settings & settings, Store & store, const Input & _input) const override
    {
        Input input(_input);
        auto path = getStrAttr(input.attrs, "path");

        auto absPath = getAbsPath(input);

        // FIXME: check whether access to 'path' is allowed.
        auto storePath = store.maybeParseStorePath(absPath.string());

        if (storePath)
            store.addTempRoot(*storePath);

        time_t mtime = 0;
        if (!storePath || storePath->name() != "source" || !store.isValidPath(*storePath)) {
            storePath = [&]() -> StorePath {
                /* Name once; a miss replays the same observation through the
                   ordinary verified ingestion. An existing destination needs
                   no store staging, holder rows, or second source walk. */
                auto accessor = makeFSSourceAccessor(absPath, /*trackLastModified=*/true);
                if (auto * local = dynamic_cast<LocalStore *>(&store);
                    local && local->materialisesFromObjects() && !local->hasValidPaths()) {
                    /* An empty store cannot hold the destination, whatever
                       this observation names. Hash and write it in one walk,
                       without retaining and replaying all input bytes. This
                       is only a choice of route: a concurrent registration
                       is handled by materialise's ordinary final check. */
                    auto copied = local->materialise(
                        "source",
                        SourcePath(accessor),
                        defaultPathFilter,
                        NoRepair,
                        [](const CanonPath &, const SourceAccessor::Stat &) { return std::nullopt; });
                    mtime = accessor->getLastModified().value();
                    return copied;
                }
                auto captured = make_ref<PreparedTree>();
                auto object = captured->capture(*accessor);
                mtime = accessor->getLastModified().value();
                auto info = ValidPathInfo::makeFromCA(
                    store,
                    "source",
                    ContentAddressWithReferences::fromParts(
                        ContentAddressMethod::Raw::Git, merkle::objectHash(object.root), {}),
                    ObjectHash::of(object.root));
                info.narSize = object.narSize;
                store.addTempRoot(info.path);
                if (!store.isValidPath(info.path)) {
                    Activity act(*logger, lvlTalkative, actUnknown, fmt("copying %s to the store", PathFmt(absPath)));
                    info.lazyNarHash = [captured] {
                        HashSink sink{HashAlgorithm::SHA256};
                        captured->dumpPath(CanonPath::root, sink);
                        return sink.finish().hash;
                    };
                    if (auto * local = dynamic_cast<LocalStore *>(&store); local && local->materialisesFromObjects()) {
                        /* The captured observation is immutable and supports
                           independent positioned reads. Use the ordinary tree
                           ingestion so sibling writes can overlap. */
                        auto copied = local->materialise(
                            "source",
                            SourcePath(captured),
                            defaultPathFilter,
                            NoRepair,
                            [](const CanonPath &, const SourceAccessor::Stat &) { return std::nullopt; });
                        if (copied != info.path)
                            throw Error("prepared source changed its content address during ingestion");
                    } else {
                        auto source =
                            sinkToSource([captured](Sink & sink) { captured->dumpPath(CanonPath::root, sink); });
                        store.addToStore(info, *source, NoRepair, NoCheckSigs);
                    }
                }
                return info.path;
            }();
        }

        auto accessor = store.requireStoreObjectAccessor(*storePath);

        // To prevent `fetchToStore()` copying the path again to Nix
        // store, pre-create an entry in the fetcher cache: the tree's
        // name, from the object hash the store holds for the path (the
        // path may have been valid before this call, so the hash is not
        // otherwise known here; a daemon without the object hash gets
        // it computed by one walk, `Store::queryObjectHash`).
        auto objectHash = store.queryObjectHash(*storePath).hash;
        accessor->fingerprint = fmt("path:%s", objectHash.to_string(HashFormat::SRI, true));
        recordRootEntry(settings, SourcePath(accessor), objectHash);

        /* Trust the lastModified value supplied by the user, if
           any. It's not a "secure" attribute so we don't care. */
        if (!input.getLastModified())
            input.attrs.insert_or_assign("lastModified", uint64_t(mtime));

        return {accessor, std::move(input)};
    }

    std::optional<ExperimentalFeature> experimentalFeature() const override
    {
        return Xp::Flakes;
    }
};

static auto rPathInputScheme = OnStartup([] { registerInputScheme(std::make_unique<PathInputScheme>()); });

} // namespace nix::fetchers
