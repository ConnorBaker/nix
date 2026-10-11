#include "nix/util/nar-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/error.hh"

namespace nix {

namespace {

struct NarAccessorImpl : FsoSourceAccessor<NarAccessorImpl, NarListing, NarAccessor>
{
private:
    void anchor() override {};
    friend struct FsoSourceAccessor<NarAccessorImpl, NarListing, NarAccessor>;

public:
    NarListing root;

    std::function<void(uint64_t, uint64_t, Sink &)> getNarBytes;

    const NarListing & getListing() const override
    {
        return root;
    }

    NarAccessorImpl(std::string && nar)
        : root{[&nar]() {
            StringSource source(nar);
            return parseNarListing(source);
        }()}
        , getNarBytes{[nar = std::move(nar)](uint64_t offset, uint64_t length, Sink & sink) {
            if (offset > nar.size() || length > nar.size() - offset)
                throw Error(
                    "reading invalid NAR bytes range: requested %1% bytes at offset %2%, but NAR has size %3%",
                    length,
                    offset,
                    nar.size());
            if (length)
                sink(std::string_view(nar.data() + offset, length));
        }}
    {
    }

    NarAccessorImpl(NarListing && listing)
        : root{std::move(listing)}
    {
    }

    NarAccessorImpl(NarListing && listing, GetNarBytes getNarBytes)
        : root{std::move(listing)}
        , getNarBytes{std::move(getNarBytes)}
    {
    }

    NarListing & require(const CanonPath & path)
    {
        auto result = find(path);
        if (!result)
            throw Error("NAR file does not contain path '%1%'", path);
        return *result;
    }

    template<typename Part>
    [[noreturn]] void wrongKind(const CanonPath & path, const NarListing &)
    {
        constexpr auto kind = std::is_same_v<Part, NarListing::Regular> ? "a regular file"
                              : std::is_same_v<Part, NarListing::Directory> ? "a directory"
                                                                         : "a symlink";
        throw Error("path '%1%' inside NAR file is not %2%", path, kind);
    }

    void deliverRegular(const NarListing::Regular & regular, Sink & sink, const fun<void(uint64_t)> & sizeCallback)
    {
        assert(getNarBytes);
        sizeCallback(regular.contents.fileSize.value());
        getNarBytes(regular.contents.narOffset.value(), regular.contents.fileSize.value(), sink);
    }

};

} // namespace

NarAccessor::~NarAccessor() {}

ref<NarAccessor> makeNarAccessor(std::string && nar)
{
    return make_ref<NarAccessorImpl>(std::move(nar));
}

ref<NarAccessor> makeNarAccessor(NarListing listing)
{
    return make_ref<NarAccessorImpl>(std::move(listing));
}

ref<NarAccessor> makeLazyNarAccessor(NarListing listing, GetNarBytes getNarBytes)
{
    return make_ref<NarAccessorImpl>(std::move(listing), getNarBytes);
}

GetNarBytes seekableGetNarBytes(const std::filesystem::path & path)
{
    auto fd = openFileReadonly(path);
    if (!fd)
        throw NativeSysError("opening NAR cache file %s", PathFmt(path));

    auto inner = seekableGetNarBytes(fd.get());
    return [inner = std::move(inner), fd = make_ref<AutoCloseFD>(std::move(fd))](
               uint64_t offset, uint64_t length, Sink & sink) { return inner(offset, length, sink); };
}

GetNarBytes seekableGetNarBytes(Descriptor fd)
{
    return [fd](uint64_t offset, uint64_t length, Sink & sink) {
        if (offset >= std::numeric_limits<off_t>::max()) /* Just in case off_t is not 64 bits. */
            throw Error("can't read %1% NAR bytes from offset %2%: offset too big", length, offset);
        if (length >= std::numeric_limits<size_t>::max()) /* Just in case size_t is 32 bits. */
            throw Error("can't read %1% NAR bytes from offset %2%: length is too big", length, offset);
        copyFdRange(fd, static_cast<off_t>(offset), static_cast<size_t>(length), sink);
    };
}

} // namespace nix
