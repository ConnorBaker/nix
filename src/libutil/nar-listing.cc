#include "nix/util/nar-listing.hh"
#include "nix/util/archive.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/error.hh"
#include "nix/util/finally.hh"

#include <utility>

namespace nix {

static NarListing parseNarListing(Source & source, FileSystemObjectSink * also)
{
    struct NarMemberConstructor : CreateRegularFileSink
    {
    private:

        NarListing::Regular & regular;

        uint64_t & pos;

    public:

        NarMemberConstructor(NarListing::Regular & reg, uint64_t & pos)
            : regular(reg)
            , pos(pos)
        {
        }

        void isExecutable() override
        {
            regular.executable = true;
        }

        void preallocateContents(uint64_t size) override
        {
            regular.contents.fileSize = size;
            regular.contents.narOffset = pos;
        }

        void operator()(std::string_view data) override {}
    };

    struct NarIndexer : NullFileSystemObjectSink, Source
    {
        std::optional<NarListing> root;
        Source & source;

        NarListing::Directory * parent = nullptr;

        uint64_t pos = 0;

        NarIndexer(Source & source)
            : source(source)
        {
        }

        NarListing & createMember(const CanonPath & path, NarListing member)
        {
            if (!parent)
                return root.emplace(std::move(member));
            return parent->entries.emplace(*path.baseName(), std::move(member)).first->second;
        }

        void createDirectory(const CanonPath & path, DirectoryCreatedCallback callback) override
        {
            auto & directory = std::get<NarListing::Directory>(createMember(path, NarListing::Directory{}).raw);
            auto previous = std::exchange(parent, &directory);
            Finally restore{[&] { parent = previous; }};
            callback(*this, path);
        }

        void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)> func) override
        {
            auto & reg = std::get<NarListing::Regular>(
                createMember(path, NarListing::Regular{.contents = {0, pos}}).raw);
            NarMemberConstructor nmc{reg, pos};
            nmc.skipContents = true; /* Don't care about contents. */
            func(nmc);
        }

        void createSymlink(const CanonPath & path, const std::string & target) override
        {
            createMember(path, NarListing::Symlink{.target = target});
        }

        size_t read(char * data, size_t len) override
        {
            auto n = source.read(data, len);
            pos += n;
            return n;
        }

        void skip(size_t len) override
        {
            source.skip(len);
            pos += len;
        }
    };

    NarIndexer indexer(source);
    if (also) {
        TeeFileSystemObjectSink both{indexer, *also};
        parseDump(both, indexer);
    } else
        parseDump(indexer, indexer);
    return std::move(*indexer.root);
}

NarListing parseNarListing(Source & source)
{
    return parseNarListing(source, nullptr);
}

NarListing parseNarListing(Source & source, FileSystemObjectSink & also)
{
    return parseNarListing(source, &also);
}

template<bool deep>
using ListNarResult = std::conditional_t<deep, NarListing, ShallowNarListing>;

template<bool deep>
static ListNarResult<deep> listNarImpl(SourceAccessor & accessor, const CanonPath & path)
{
    auto st = accessor.lstat(path);

    switch (st.type) {
    case SourceAccessor::Type::tRegular:
        return typename ListNarResult<deep>::Regular{
            .executable = st.isExecutable,
            .contents =
                NarListingRegularFile{
                    .fileSize = st.fileSize,
                    .narOffset = st.narOffset && *st.narOffset ? st.narOffset : std::nullopt,
                },
        };
    case SourceAccessor::Type::tDirectory: {
        typename ListNarResult<deep>::Directory dir;
        for (const auto & [name, type] : accessor.readDirectory(path)) {
            if constexpr (deep) {
                dir.entries.emplace(name, listNarImpl<true>(accessor, path / name));
            } else {
                dir.entries.emplace(name, fso::Opaque{});
            }
        }
        return dir;
    }
    case SourceAccessor::Type::tSymlink:
        return typename ListNarResult<deep>::Symlink{
            .target = accessor.readLink(path),
        };
    case SourceAccessor::Type::tBlock:
    case SourceAccessor::Type::tChar:
    case SourceAccessor::Type::tSocket:
    case SourceAccessor::Type::tFifo:
    case SourceAccessor::Type::tUnknown:
    default:
        throw Error("file '%s' has an unsupported type", accessor.showPath(path));
    }
}

NarListing listNarDeep(SourceAccessor & accessor, const CanonPath & path)
{
    return listNarImpl<true>(accessor, path);
}

ShallowNarListing listNarShallow(SourceAccessor & accessor, const CanonPath & path)
{
    return listNarImpl<false>(accessor, path);
}

} // namespace nix
