#include "nix/fetchers/fetchers.hh"
#include "nix/store/store-api.hh"
#include "nix/util/archive.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/merkle-hash.hh"

namespace nix::fetchers {

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
        auto accessor = [&]() -> ref<SourceAccessor> {
            if (!storePath || storePath->name() != "source" || !store.isValidPath(*storePath)) {
                auto source = makeFSSourceAccessor(absPath, /*trackLastModified=*/true);
                auto acquired = GitRepo::acquire(SourcePath{source});
                auto objectHash = merkle::objectHash(*acquired->getKnownEntry(CanonPath::root));
                acquired->fingerprint = fmt("path:%s", objectHash.to_string(HashFormat::SRI, true));
                auto copied = fetchToStore2(settings, store, SourcePath{acquired}, FetchMode::Copy, "source");
                acquired->setPathDisplay(store.printStorePath(copied.first));
                mtime = source->getLastModified().value();
                return acquired;
            }

            return store.requireStoreObjectAccessor(*storePath);
        }();

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
