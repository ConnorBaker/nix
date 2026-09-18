#include <algorithm>
#include <cerrno>
#include <limits>
#include <map>
#include <variant>

#include <strings.h> // for strcasecmp

#include "nix/util/archive.hh"
#include "nix/util/config-global.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/source-path.hh"
#include "nix/util/file-system.hh"
#include "nix/util/signals.hh"
#include "nix/util/tree-traversal.hh"

namespace nix {

struct ArchiveSettings : Config
{
private:
    void anchor() override;
public:

    Setting<bool> useCaseHack{
        this,
#ifdef __APPLE__
        true,
#else
        false,
#endif
        "use-case-hack",
        "Whether to enable a macOS-specific hack for dealing with file name case collisions."};
};

static ArchiveSettings archiveSettings;

static GlobalConfig::Register rArchiveSettings(&archiveSettings);

PathFilter defaultPathFilter = [](const std::string &) { return true; };

std::string_view unhackName(std::string_view name)
{
    size_t pos = name.find(caseHackSuffix);
    if (pos == std::string_view::npos)
        return name;
    /* With the hack off nothing strips the suffix, so the name would be
       serialised as it is and refused by every parser (`parse` below). */
    if (!archiveSettings.useCaseHack)
        throw Error(
            "entry '%s' carries the case-hack suffix '%s', which the NAR format reserves", name, caseHackSuffix);
    return name.substr(0, pos);
}

StringMap unhackedEntries(SourceAccessor & accessor, const CanonPath & path)
{
    StringMap unhacked;
    for (auto & i : accessor.readDirectory(path)) {
        if (!archiveSettings.useCaseHack && i.first.find(caseHackSuffix) != std::string::npos)
            throw Error(
                "cannot serialise '%s': entry '%s' carries the case-hack suffix, which the NAR format reserves",
                accessor.showPath(path),
                i.first);
        std::string name(unhackName(i.first));
        if (name.size() != i.first.size())
            debug("removing case hack suffix from '%s'", path / i.first);
        if (!unhacked.emplace(name, i.first).second)
            throw Error("file name collision between '%s' and '%s'", (path / unhacked[name]), (path / i.first));
    }
    return unhacked;
}

std::set<CanonPath> filteredPaths(SourceAccessor & accessor, const CanonPath & path, PathFilter & filter)
{
    /* Every node the walk reaches is in the set; a file or symlink is
       "known" so that its contents are not read, a directory is descended. */
    struct Collector : NodeVisitor<std::monostate>
    {
        const CanonPath & root;
        std::set<CanonPath> paths;

        explicit Collector(const CanonPath & root)
            : root(root)
        {
        }

        std::optional<std::monostate> known(const CanonPath & path, const SourceAccessor::Stat & st) override
        {
            paths.insert(root / path);
            if (st.type == SourceAccessor::tDirectory)
                return std::nullopt;
            return std::monostate{};
        }

        std::monostate regular(const CanonPath &, fun<void(CreateRegularFileSink &)>) override
        {
            return {};
        }

        std::monostate symlink(const CanonPath &, const std::string &) override
        {
            return {};
        }

        std::monostate directory(const CanonPath &, Children children) override
        {
            children([](std::string_view, fun<std::monostate()> visit) { visit(); });
            return {};
        }
    };

    Collector collector{path};
    traverse(accessor, path, filter, collector);
    return std::move(collector.paths);
}

namespace {

/* The NAR serialisation of each node, in the format `archive.hh` states;
   the traversal supplies the order and the names. */
struct NarWriter : NodeVisitor<std::monostate>
{
    Sink & sink;

    explicit NarWriter(Sink & sink)
        : sink(sink)
    {
    }

    std::monostate regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) override
    {
        struct Contents : CreateRegularFileSink
        {
            Sink & sink;
            std::optional<uint64_t> size;

            explicit Contents(Sink & sink)
                : sink(sink)
            {
            }

            void isExecutable() override
            {
                sink << "executable" << "";
            }

            void preallocateContents(uint64_t announced) override
            {
                size = announced;
                sink << "contents" << announced;
            }

            void operator()(std::string_view data) override
            {
                sink(data);
            }
        };

        sink << "(" << "type" << "regular";
        Contents contents{sink};
        read(contents);
        if (!contents.size)
            throw Error("cannot serialise '%s': its size was not announced before its contents", path);
        writePadding(*contents.size, sink);
        sink << ")";
        return {};
    }

    std::monostate symlink(const CanonPath &, const std::string & target) override
    {
        sink << "(" << "type" << "symlink" << "target" << target << ")";
        return {};
    }

    std::monostate directory(const CanonPath &, Children children) override
    {
        sink << "(" << "type" << "directory";
        children([&](std::string_view name, fun<std::monostate()> visit) {
            sink << "entry" << "(" << "name" << name << "node";
            visit();
            sink << ")";
        });
        sink << ")";
        return {};
    }
};

} // namespace

void SourceAccessor::dumpPath(const CanonPath & path, Sink & sink, PathFilter & filter)
{
    sink << narVersionMagic1;
    NarWriter writer{sink};
    traverse(*this, path, filter, writer);
}

void ArchiveSettings::anchor() {}

time_t dumpPathAndGetMtime(const std::filesystem::path & path, Sink & sink, PathFilter & filter)
{
    SourcePath path2 = makeFSSourceAccessor(absPath(path), /*trackLastModified=*/true);
    path2.dumpPath(sink, filter);
    return path2.accessor->getLastModified().value();
}

void dumpPath(const std::filesystem::path & path, Sink & sink, PathFilter & filter)
{
    SourcePath path2 = makeFSSourceAccessor(absPath(path), /*trackLastModified=*/false);
    path2.dumpPath(sink, filter);
}

void dumpString(std::string_view s, Sink & sink)
{
    sink << narVersionMagic1 << "(" << "type" << "regular" << "contents" << s << ")";
}

template<typename... Args>
static SerialisationError badArchive(std::string_view s, Args &&... args)
{
    return SerialisationError("bad archive: " + s, std::forward<Args>(args)...);
}

static void parseContents(CreateRegularFileSink & sink, Source & source)
{
    uint64_t size = readLongLong(source);

    sink.preallocateContents(size);

    if (sink.skipContents) {
        uint64_t left = size;
        /* Source::skip takes a size_t, which might be narrower on 32 bit systems, so
           be careful around truncations. */
        while (left) {
            size_t toSkip = std::min<uint64_t>(left, std::numeric_limits<size_t>::max());
            source.skip(toSkip);
            left -= toSkip;
        }
    } else {
        source.drainInto(sink, size);
    }

    readPadding(size, source);
}

CaseHackCollision::CaseHackCollision(std::string name_, std::string existing_)
    : Error("file name '%s' collides with case-hacked file name '%s'", name_, existing_)
    , name(std::move(name_))
    , existing(std::move(existing_))
{
}

CaseHackCollision::~CaseHackCollision() = default;

bool CaseHackNames::CaseInsensitiveCompare::operator()(const std::string & a, const std::string & b) const
{
    return strcasecmp(a.c_str(), b.c_str()) < 0;
}

std::string CaseHackNames::diskName(std::string_view name0)
{
    std::string name(name0);
    if (!archiveSettings.useCaseHack)
        return name;
    auto i = names.find(name);
    if (i == names.end()) {
        names[name] = 0;
        return name;
    }
    debug("case collision between '%1%' and '%2%'", i->first, name);
    name += caseHackSuffix;
    name += std::to_string(++i->second);
    /* Reachable: the suffix check of `parseDump` is case-sensitive, as
       `unhackName` is, so an entry spelling the suffix in another case is
       admitted, and on a case-insensitive file system it is the file the
       hacked name would overwrite. */
    if (auto j = names.find(name); j != names.end())
        throw CaseHackCollision(std::string(name0), j->first);
    return name;
}

static void parse(FileSystemObjectSink & sink, Source & source, const CanonPath & path, size_t depth)
{
    if (depth >= narMaxDepth)
        throw badArchive("NAR directory nesting exceeds maximum depth of %d", narMaxDepth);

    /* NAR keywords are all <= 10 bytes; a little slack keeps error
       messages useful for short garbage without allowing large
       allocations. */
    constexpr size_t narMaxTag = 32;
    /* Format-defined bounds, intentionally independent of host
       NAME_MAX/PATH_MAX. */
    constexpr size_t narMaxName = 255;
    constexpr size_t narMaxTarget = 4095;

    auto getString = [&](size_t max) {
        checkInterrupt();
        return readString(source, max);
    };

    auto expectTag = [&](std::string_view expected) {
        auto tag = getString(narMaxTag);
        if (tag != expected)
            throw badArchive("expected tag '%s', got '%s'", expected, tag);
    };

    expectTag("(");

    expectTag("type");

    auto type = getString(narMaxTag);

    if (type == "regular") {
        sink.createRegularFile(path, [&](auto & crf) {
            auto tag = getString(narMaxTag);

            if (tag == "executable") {
                auto s2 = getString(0);
                if (s2 != "")
                    throw badArchive("executable marker has non-empty value");
                crf.isExecutable();
                tag = getString(narMaxTag);
            }

            if (tag != "contents")
                throw badArchive("expected tag 'contents', got '%s'", tag);

            parseContents(crf, source);

            expectTag(")");
        });
    }

    else if (type == "directory") {
        sink.createDirectory(path, [&](FileSystemObjectSink & dirSink, const CanonPath & relDirPath) {
            CaseHackNames names;

            std::string prevName;

            while (1) {
                auto tag = getString(narMaxTag);

                if (tag == ")")
                    break;

                if (tag != "entry")
                    throw badArchive("expected tag 'entry' or ')', got '%s'", tag);

                expectTag("(");

                expectTag("name");

                auto name = getString(narMaxName);
                if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos
                    || name.find((char) 0) != std::string::npos)
                    throw badArchive("NAR contains invalid file name '%1%'", name);
                /* On every platform, whether or not `use-case-hack` is on:
                   the suffix is this parser's own marker on a case-colliding
                   entry (below), removed again by `unhackName` only where
                   the hack is on, so a name carrying it would be one name
                   here and another there, and the object hash of the NAR
                   would differ by platform. */
                if (name.find(caseHackSuffix) != std::string::npos)
                    throw badArchive(
                        "NAR contains file name '%s' with the case-hack suffix '%s'", name, caseHackSuffix);
                if (name <= prevName)
                    throw badArchive("NAR directory is not sorted");
                prevName = name;
                /* The case hack's renaming, one rule for every writer of
                   a tree (`CaseHackNames`). */
                try {
                    name = names.diskName(name);
                } catch (CaseHackCollision & e) {
                    throw badArchive(
                        "NAR contains file name '%s' that collides with case-hacked file name '%s'",
                        e.name,
                        e.existing);
                }

                expectTag("node");

                parse(dirSink, source, relDirPath / name, depth + 1);

                expectTag(")");
            }
        });
    }

    else if (type == "symlink") {
        expectTag("target");

        auto target = getString(narMaxTarget);
        if (target.empty() || target.find((char) 0) != std::string::npos)
            throw badArchive("NAR contains invalid symlink target");
        sink.createSymlink(path, target);

        expectTag(")");
    }

    else
        throw badArchive("unknown file type '%s'", type);
}

void parseDump(FileSystemObjectSink & sink, Source & source)
{
    std::string version;
    try {
        version = readString(source, narVersionMagic1.size());
    } catch (SerialisationError & e) {
        /* This generally means the integer at the start couldn't be
           decoded.  Ignore and throw the exception below. */
    }
    if (version != narVersionMagic1)
        throw badArchive("input doesn't look like a Nix archive");
    parse(sink, source, CanonPath::root, 0);
}

void restorePath(const std::filesystem::path & path, Source & source, bool startFsync, RestoreSinkHooks * hooks)
{
    RestoreSink sink{startFsync, hooks};
    sink.dstPath = path;
    parseDump(sink, source);
}

void copyNAR(Source & source, Sink & sink)
{
    // FIXME: if 'source' is the output of dumpPath() followed by EOF,
    // we should just forward all data directly without parsing.

    NullFileSystemObjectSink parseSink; /* just parse the NAR */

    TeeSource wrapper{source, sink};

    parseDump(parseSink, wrapper);
}

} // namespace nix
