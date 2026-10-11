#include "nix/util/memory-source-accessor.hh"

#include <ranges>

namespace nix {

void MemorySink::anchor() {}

void MemorySourceAccessor::anchor() {}

MemorySourceAccessor::File * MemorySourceAccessor::open(const CanonPath & path, std::optional<File> create)
{
    if (!root) {
        if (!path.isRoot() || !create)
            return nullptr;
        root = std::move(*create);
        return &root.value();
    }

    File * cur = &root.value();

    bool newF = false;

    unsigned j = 0;
    for (std::string_view name : path) {
        auto * curDirP = std::get_if<File::Directory>(&cur->raw);
        if (!curDirP) {
            CanonPath curDirPath = CanonPath::root;
            for (auto name2 : std::views::take(path, j))
                curDirPath.push(name2);
            if (std::holds_alternative<File::Symlink>(cur->raw))
                throw SymlinkNotAllowed(curDirPath, "file '%s' is a symlink", showPath(curDirPath));
            if (create)
                throw NotADirectory("file '%s' is not a directory", showPath(curDirPath));
            return nullptr;
        }

        auto & curDir = *curDirP;

        auto i = curDir.entries.find(name);
        if (i == curDir.entries.end()) {
            if (!create)
                return nullptr;
            else {
                newF = true;
                i = curDir.entries.insert(
                    i,
                    {
                        std::string{name},
                        File::Directory{},
                    });
            }
        }
        cur = &i->second;
        ++j;
    }

    if (newF && create)
        *cur = std::move(*create);

    return cur;
}

bool MemorySourceAccessor::pathExists(const CanonPath & path)
{
    return open(path, std::nullopt);
}


MemorySourceAccessor::File & MemorySourceAccessor::create(
    const CanonPath & path, File && initial, std::string_view noun, std::string_view description)
{
    File * file = nullptr;
    try {
        file = open(path, std::move(initial));
        if (!file)
            throw Error(
                "%s '%s' cannot be created because some parent directories don't exist",
                Uncolored(noun), showPath(path));
    } catch (SourceAccessorError & e) {
        e.addTrace({}, "while creating %s '%s'", Uncolored(description), showPath(path));
        throw;
    }
    return *file;
}

SourcePath MemorySourceAccessor::addFile(CanonPath path, std::string && contents)
{
    // Create root directory automatically if necessary as a convenience.
    if (!root && !path.isRoot())
        open(CanonPath::root, File::Directory{});

    auto & file = create(path, File{File::Regular{}}, "file", "file");
    if (auto * r = std::get_if<File::Regular>(&file.raw))
        r->contents = std::move(contents);
    else
        throw NotARegularFile("file '%s' is not a regular file", showPath(path));

    return SourcePath{ref(shared_from_this()), path};
}

using File = MemorySourceAccessor::File;

void MemorySink::createDirectory(const CanonPath & path)
{
    auto & file = dst.create(path, File{File::Directory{}}, "directory", "directory");
    if (!std::holds_alternative<File::Directory>(file.raw))
        throw NotADirectory("file '%s' is not a directory", dst.showPath(path));
};

struct CreateMemoryRegularFile : CreateRegularFileSink
{
    File::Regular & regularFile;

    CreateMemoryRegularFile(File::Regular & r)
        : regularFile(r)
    {
    }

    void operator()(std::string_view data) override;
    void isExecutable() override;
    void preallocateContents(uint64_t size) override;
};

void MemorySink::createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)> func)
{
    auto & file = dst.create(path, File{File::Regular{}}, "file", "regular file");
    if (auto * regular = std::get_if<File::Regular>(&file.raw)) {
        CreateMemoryRegularFile crf{*regular};
        func(crf);
    } else
        throw NotARegularFile("file '%s' is not a regular file", dst.showPath(path));
}

void CreateMemoryRegularFile::isExecutable()
{
    regularFile.executable = true;
}

void CreateMemoryRegularFile::preallocateContents(uint64_t len)
{
    if (len > std::numeric_limits<decltype(regularFile.contents)::size_type>::max())
        throw Error("cannot preallocate contents for a file that is too large to fit in memory");
    regularFile.contents.reserve(len);
}

void CreateMemoryRegularFile::operator()(std::string_view data)
{
    regularFile.contents += data;
}

void MemorySink::createSymlink(const CanonPath & path, const std::string & target)
{
    auto & file = dst.create(path, File{File::Symlink{}}, "symlink", "symlink");
    if (auto * symlink = std::get_if<File::Symlink>(&file.raw))
        symlink->target = target;
    else
        throw NotASymlink("file '%s' is not a symbolic link", dst.showPath(path));
}

ref<SourceAccessor> makeEmptySourceAccessor()
{
    static auto empty = []() {
        auto empty = make_ref<MemorySourceAccessor>();
        MemorySink sink{*empty};
        sink.createDirectory(CanonPath::root);
        /* Don't forget to clear the display prefix, as the default constructed
           SourceAccessor has the «unknown» prefix. Since this accessor is supposed
           to mimic an empty root directory the prefix needs to be empty. */
        empty->setPathDisplay("");
        return empty.cast<SourceAccessor>();
    }();
    return empty;
}

} // namespace nix
