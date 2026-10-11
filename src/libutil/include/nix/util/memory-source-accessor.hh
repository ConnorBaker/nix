#pragma once
///@file

#include "nix/util/source-path.hh"
#include "nix/util/serialise.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/variant-wrapper.hh"
#include "nix/util/util.hh"
#include "nix/util/json-impls.hh"

namespace nix {

/**
 * File System Object definitions
 *
 * @see https://nix.dev/manual/nix/latest/store/file-system-object.html
 */
namespace fso {

template<typename RegularContents>
struct Regular
{
    bool executable = false;
    RegularContents contents;

    auto operator<=>(const Regular &) const = default;
};

/**
 * Child parameter because sometimes we want "shallow" directories without
 * full file children.
 */
template<typename Child>
struct DirectoryT
{
    using Name = std::string;

    std::map<Name, Child, std::less<>> entries;

    inline bool operator==(const DirectoryT &) const noexcept;
    inline std::strong_ordering operator<=>(const DirectoryT &) const noexcept;
};

template<typename Contents>
struct SymlinkT
{
    Contents target;

    auto operator<=>(const SymlinkT &) const = default;
};

using Symlink = SymlinkT<std::string>;

/**
 * For when we know there is child, but don't know anything about it.
 *
 * This is not part of the core File System Object data model --- this
 * represents not knowing, not an additional type of file.
 */
struct Opaque
{
    auto operator<=>(const Opaque &) const = default;
};

/**
 * `File<std::string>` nicely defining what a "file system object"
 * is in Nix.
 *
 * With a different type arugment, it is also can be a "skeletal"
 * version is that abstract syntax for a "NAR listing".
 */
template<typename RegularContents, bool recur, typename Metadata = std::monostate,
         typename SymlinkContents = std::string>
struct VariantT
{
    bool operator==(const VariantT &) const noexcept;
    std::strong_ordering operator<=>(const VariantT &) const noexcept;

    using Regular = nix::fso::Regular<RegularContents>;

    /**
     * In the default case, we do want full file children for our directory.
     */
    using Directory = nix::fso::DirectoryT<std::conditional_t<recur, VariantT, Opaque>>;

    using Symlink = nix::fso::SymlinkT<SymlinkContents>;

    using Raw = std::variant<Regular, Directory, Symlink>;
    Raw raw;
    [[no_unique_address]] Metadata metadata{};

    MAKE_WRAPPER_CONSTRUCTOR(VariantT);

    VariantT(std::pair<Raw, Metadata> value)
        : raw(std::move(value.first)), metadata(std::move(value.second))
    {
    }

    SourceAccessor::Stat lstat() const;
};

template<typename Child>
inline bool DirectoryT<Child>::operator==(const DirectoryT &) const noexcept = default;

template<typename Child>
inline std::strong_ordering DirectoryT<Child>::operator<=>(const DirectoryT &) const noexcept = default;

template<typename RegularContents, bool recur, typename Metadata, typename SymlinkContents>
inline bool
VariantT<RegularContents, recur, Metadata, SymlinkContents>::operator==(
    const VariantT<RegularContents, recur, Metadata, SymlinkContents> &) const noexcept = default;

template<typename RegularContents, bool recur, typename Metadata, typename SymlinkContents>
inline std::strong_ordering
VariantT<RegularContents, recur, Metadata, SymlinkContents>::operator<=>(
    const VariantT<RegularContents, recur, Metadata, SymlinkContents> &) const noexcept = default;

template<typename RegularContents, bool recur, typename Metadata, typename SymlinkContents>
SourceAccessor::Stat VariantT<RegularContents, recur, Metadata, SymlinkContents>::lstat() const
{
    return std::visit(
        overloaded{
            [](const Regular & r) {
                SourceAccessor::Stat st{.type = SourceAccessor::tRegular, .isExecutable = r.executable};
                if constexpr (requires { r.contents.fileSize; r.contents.narOffset; }) {
                    st.fileSize = r.contents.fileSize;
                    st.narOffset = r.contents.narOffset;
                } else
                    st.fileSize = r.contents.size();
                return st;
            },
            [](const Directory &) { return SourceAccessor::Stat{.type = SourceAccessor::tDirectory}; },
            [](const Symlink &) { return SourceAccessor::Stat{.type = SourceAccessor::tSymlink}; },
        },
        raw);
}

} // namespace fso

struct FsoVirtualSourceAccessor : virtual SourceAccessor
{
    bool operator==(const FsoVirtualSourceAccessor &) const noexcept = default;
};

template<typename Derived, typename Node = fso::VariantT<std::string, true>,
         typename Base = FsoVirtualSourceAccessor>
struct FsoSourceAccessor : Base
{
    using File = Node;
    bool operator==(const FsoSourceAccessor &) const noexcept = default;
    using Base::readFile;

protected:
    Derived & self() { return static_cast<Derived &>(*this); }

    auto find(const CanonPath & path)
    {
        auto * current = &self().root;
        for (auto name : path) {
            auto directory = std::get_if<typename File::Directory>(&current->raw);
            if (!directory)
                return decltype(current){};
            auto child = nix::get(directory->entries, name);
            if (!child)
                return decltype(current){};
            current = child;
        }
        return current;
    }

    decltype(auto) require(const CanonPath & path)
    {
        if (auto file = self().find(path))
            return *file;
        throw FileNotFound("file '%s' does not exist", this->showPath(path));
    }

    template<typename Part, typename Value>
    [[noreturn]] void wrongKind(const CanonPath & path, const Value & value)
    {
        if constexpr (!std::is_same_v<Part, typename File::Symlink>)
            if (value.raw.valueless_by_exception())
                std::visit([](const auto &) {}, value.raw);
        if constexpr (!std::is_same_v<Part, typename File::Symlink>)
            if (std::holds_alternative<typename File::Symlink>(value.raw))
                throw SymlinkNotAllowed(path, "file '%s' is a symlink", this->showPath(path));
        if constexpr (std::is_same_v<Part, typename File::Regular>)
            throw NotARegularFile("file '%s' is not a regular file", this->showPath(path));
        else if constexpr (std::is_same_v<Part, typename File::Directory>)
            throw NotADirectory("file '%s' is not a directory", this->showPath(path));
        else
            throw NotASymlink("file '%s' is not a symbolic link", this->showPath(path));
    }

    template<typename Part>
    auto & part(const CanonPath & path)
    {
        auto & file = self().require(path);
        if (auto value = std::get_if<Part>(&file.raw))
            return *value;
        self().template wrongKind<Part>(path, file);
    }

    void deliverRegular(const typename File::Regular & regular, Sink & sink, const fun<void(uint64_t)> & sizeCallback)
    {
        sizeCallback(regular.contents.size());
        if constexpr (
            std::is_same_v<decltype(regular.contents), std::string_view>
            || std::is_same_v<decltype(regular.contents), std::string>) {
            if (std::is_same_v<decltype(regular.contents), std::string_view>
                || static_cast<SourceAccessor &>(*this).isFixed()) {
                if (!regular.contents.empty())
                    sink(regular.contents);
                return;
            }
        }
        StringSource source{regular.contents};
        source.drainInto(sink);
    }

public:
    std::optional<SourceAccessor::Stat> maybeLstat(const CanonPath & path) override
    {
        auto file = self().find(path);
        return file ? std::optional{file->lstat()} : std::nullopt;
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        auto keepAlive = this->weak_from_this().lock();
        self().deliverRegular(part<typename File::Regular>(path), sink, sizeCallback);
    }

    SourceAccessor::DirEntries readDirectory(const CanonPath & path) override
    {
        auto & directory = part<typename File::Directory>(path);
        SourceAccessor::DirEntries result;
        for (auto & [name, file] : directory.entries)
            result.emplace_hint(result.end(), name, file.lstat().type);
        return result;
    }

    std::string readLink(const CanonPath & path) override
    {
        return std::string(part<typename File::Symlink>(path).target);
    }
};

/**
 * An source accessor for an in-memory file system.
 */
struct MemorySourceAccessor : FsoSourceAccessor<MemorySourceAccessor>
{
private:
    void anchor() override;
    friend struct FsoSourceAccessor<MemorySourceAccessor>;
    File * find(const CanonPath & path) { return open(path, std::nullopt); }

public:
    using File = fso::VariantT<std::string, true>;

    std::optional<File> root;

    bool operator==(const MemorySourceAccessor &) const noexcept = default;

    bool operator<(const MemorySourceAccessor & other) const noexcept
    {
        return root < other.root;
    }

    using FsoSourceAccessor<MemorySourceAccessor>::readFile;
    bool pathExists(const CanonPath & path) override;

    /**
     * @param create If present, create this file and any parent directories
     * that are needed.
     *
     * Return null if
     *
     * - `create = false`: File does not exist.
     *
     * - `create = true`: some parent file was not a dir, so couldn't
     *   look/create inside.
     */
    File * open(const CanonPath & path, std::optional<File> create);

    /**
     * @brief Insert a new regular file into with the specified @p contents.
     * @todo Have a way to insert "borrowed" std::string_view without copying.
     */
    SourcePath addFile(CanonPath path, std::string && contents);

private:
    friend struct MemorySink;
    File & create(const CanonPath & path, File && initial, std::string_view noun, std::string_view description);
};

/**
 * Write to a `MemorySourceAccessor` at the given path
 */
struct MemorySink : FileSystemObjectSink
{
private:
    void anchor() override;

public:
    MemorySourceAccessor & dst;

    MemorySink(MemorySourceAccessor & dst)
        : dst(dst)
    {
    }

    void createDirectory(const CanonPath & path) override;

    void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)>) override;

    void createSymlink(const CanonPath & path, const std::string & target) override;
};

template<>
struct json_avoids_null<MemorySourceAccessor::File::Regular> : std::true_type
{};

template<>
struct json_avoids_null<MemorySourceAccessor::File::Directory> : std::true_type
{};

template<>
struct json_avoids_null<MemorySourceAccessor::File::Symlink> : std::true_type
{};

template<>
struct json_avoids_null<MemorySourceAccessor::File> : std::true_type
{};

template<>
struct json_avoids_null<MemorySourceAccessor> : std::true_type
{};

} // namespace nix

namespace nlohmann {

#define ARG nix::fso::Regular<RegularContents>
template<typename RegularContents>
JSON_IMPL_INNER(ARG);
#undef ARG

#define ARG nix::fso::DirectoryT<Child>
template<typename Child>
JSON_IMPL_INNER(ARG);
#undef ARG

template<>
JSON_IMPL_INNER(nix::fso::Symlink);

template<>
JSON_IMPL_INNER(nix::fso::Opaque);

#define ARG nix::fso::VariantT<RegularContents, recur>
template<typename RegularContents, bool recur>
JSON_IMPL_INNER(ARG);
#undef ARG

} // namespace nlohmann

JSON_IMPL(nix::MemorySourceAccessor)
