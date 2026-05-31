#include "nix/util/environment-variables.hh"
#include "nix/util/error.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/util/users.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/projection.hh"
#include "nix/store/store-api.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/os-string.hh"
#include "nix/util/processes.hh"
#include "nix/util/git.hh"
#include "nix/fetchers/git-promisor.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/fetchers/source-view-git.hh"
#include "nix/util/logging.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/json-utils.hh"
#include "nix/util/archive.hh"
#include "nix/util/memo.hh"
#include "nix/util/switch-source-accessor.hh"
#include "nix/util/sync.hh"

#include <chrono>
#include <unordered_map>

#include <sys/time.h>

#ifndef _WIN32
#  include <sys/wait.h>
#endif

using namespace std::string_literals;

namespace nix::fetchers {

namespace {

static bool isCacheFileWithinTtl(const Settings & settings, time_t now, const PosixStat & st)
{
    return st.st_mtime + static_cast<time_t>(settings.tarballTtl) > now;
}

std::filesystem::path getCachePath(std::string_view key, bool shallow)
{
    auto name =
        hashString(HashAlgorithm::SHA256, key).to_string(HashFormat::Nix32, false) + (shallow ? "-shallow" : "");
    return getCacheDir() / "gitv3" / std::move(name);
}

/* ---------- per-URL protocol-v2 capability cache ----------
 *
 * Probing the remote's `info/refs?service=git-upload-pack` for v2
 * capabilities is one HTTP round-trip per URL. We cache the result
 * for `kV2ProbeTtl` so multiple inputs against the same partial-
 * clone remote (e.g. nixpkgs across many inputs) share a single
 * probe within a process.
 */

constexpr std::chrono::minutes kV2ProbeTtl{30};

/* A *transient* probe failure (network/auth blip, not an authoritative
   "no filter") is cached only briefly: long enough to coalesce a burst
   of inputs in one evaluation, short enough that a later retry re-probes
   rather than being stuck with a false negative for the full TTL. */
constexpr std::chrono::seconds kV2ProbeFailureTtl{5};

static Sync<std::unordered_map<std::string, V2ProbeResult>> v2ProbeCache_;

/* The default probe just constructs a provider and asks it. Tests
   can override this via `setV2ProbeForTest` to inject a stub. */
static V2ProbeFn & v2ProbeFnSlot()
{
    static V2ProbeFn fn = [](const std::string & url) -> bool {
        return makeGitPromisorProvider(url, {})->supportsFilteredFetch();
    };
    return fn;
}

// Returns the name of the HEAD branch.
//
// Returns the head branch name as reported by git ls-remote --symref, e.g., if
// ls-remote returns the output below, "main" is returned based on the ref line.
//
//   ref: refs/heads/main       HEAD
//   ...
std::optional<std::string> readHead(const std::filesystem::path & path)
{
    auto [status, output] = runProgram(
        RunOptions{
            .program = "git",
            // FIXME: use 'HEAD' to avoid returning all refs
            .args = {OS_STR("ls-remote"), OS_STR("--symref"), path.native()},
            .isInteractive = true,
        });
    if (status != 0)
        return std::nullopt;

    std::string_view line = output;
    line = line.substr(0, line.find("\n"));
    if (const auto parseResult = git::parseLsRemoteLine(line); parseResult && parseResult->reference == "HEAD") {
        switch (parseResult->kind) {
        case git::LsRemoteRefLine::Kind::Symbolic:
            debug("resolved HEAD ref '%s' for repo %s", parseResult->target, PathFmt(path));
            break;
        case git::LsRemoteRefLine::Kind::Object:
            debug("resolved HEAD rev '%s' for repo %s", parseResult->target, PathFmt(path));
            break;
        }
        return parseResult->target;
    }
    return std::nullopt;
}

// Persist the HEAD ref from the remote repo in the local cached repo.
bool storeCachedHead(const std::string & actualUrl, bool shallow, const std::string & headRef)
{
    std::filesystem::path cacheDir = getCachePath(actualUrl, shallow);
    try {
        runProgram(
            "git",
            true,
            {
                OS_STR("-C"),
                cacheDir.native(),
                OS_STR("--git-dir"),
                OS_STR("."),
                OS_STR("symbolic-ref"),
                OS_STR("--"),
                OS_STR("HEAD"),
                string_to_os_string(headRef),
            });
    } catch (ExecError & e) {
        if (
#ifndef WIN32 // TODO abstract over exit status handling on Windows
            !WIFEXITED(e.status)
#else
            e.status != 0
#endif
        )
            throw;

        return false;
    }
    /* No need to touch refs/HEAD, because `git symbolic-ref` updates the mtime. */
    return true;
}

static std::optional<std::string> readHeadCached(const Settings & settings, const std::string & actualUrl, bool shallow)
{
    // Create a cache path to store the branch of the HEAD ref. Append something
    // in front of the URL to prevent collision with the repository itself.
    std::filesystem::path cacheDir = getCachePath(actualUrl, shallow);
    std::filesystem::path headRefFile = cacheDir / "HEAD";

    time_t now = time(nullptr);
    auto st = maybeStat(headRefFile);
    std::optional<std::string> cachedRef;
    if (st) {
        cachedRef = readHead(cacheDir);
        if (cachedRef != std::nullopt && isCacheFileWithinTtl(settings, now, *st)) {
            debug("using cached HEAD ref '%s' for repo '%s'", *cachedRef, actualUrl);
            return cachedRef;
        }
    }

    auto ref = readHead(actualUrl);
    if (ref)
        return ref;

    if (cachedRef) {
        // If the cached git ref is expired in fetch() below, and the 'git fetch'
        // fails, it falls back to continuing with the most recent version.
        // This function must behave the same way, so we return the expired
        // cached ref here.
        warn("could not get HEAD ref for repository '%s'; using expired cached ref '%s'", actualUrl, *cachedRef);
        return *cachedRef;
    }

    return std::nullopt;
}

std::vector<PublicKey> getPublicKeys(const Attrs & attrs)
{
    std::vector<PublicKey> publicKeys;
    if (attrs.contains("publicKeys")) {
        auto pubKeysJson = nlohmann::json::parse(getStrAttr(attrs, "publicKeys"));
        auto & pubKeys = getArray(pubKeysJson);

        for (auto & key : pubKeys) {
            publicKeys.push_back(key);
        }
    }
    if (attrs.contains("publicKey"))
        publicKeys.push_back(
            PublicKey{maybeGetStrAttr(attrs, "keytype").value_or("ssh-ed25519"), getStrAttr(attrs, "publicKey")});
    return publicKeys;
}

} // end namespace

V2ProbeResult gitV2CapsCache(const std::string & url)
{
    /* First, try to read from the cache. */
    {
        auto cache(v2ProbeCache_.lock());
        auto it = cache->find(url);
        if (it != cache->end()) {
            auto age = std::chrono::steady_clock::now() - it->second.probedAt;
            /* Authoritative results live for the full TTL; a transient
               failure expires quickly so a retry re-probes. */
            auto ttl = it->second.probeFailed ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                           kV2ProbeFailureTtl)
                                              : std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                    kV2ProbeTtl);
            if (age < ttl)
                return it->second;
        }
    }

    /* Probe (potentially network IO; runs without the cache lock). */
    bool supports = false;
    bool failed = false;
    try {
        supports = v2ProbeFnSlot()(url);
    } catch (Error & e) {
        debug("partial-clone capability probe for '%s' failed (transient — short TTL): %s", url, e.what());
        failed = true;
    }

    V2ProbeResult result{
        .supportsFilter = supports,
        .probeFailed = failed,
        .probedAt = std::chrono::steady_clock::now(),
    };

    {
        auto cache(v2ProbeCache_.lock());
        cache->insert_or_assign(url, result);
    }

    return result;
}

V2ProbeFn setV2ProbeForTest(V2ProbeFn fn)
{
    auto & slot = v2ProbeFnSlot();
    auto prev = std::move(slot);
    slot = std::move(fn);
    return prev;
}

void gitV2CapsCacheClearForTest()
{
    auto cache(v2ProbeCache_.lock());
    cache->clear();
}

static const Hash nullRev{HashAlgorithm::SHA1};

static LazyAttr makeLazyAttr(fun<ResolvedAttr()> compute)
{
    return make_ref<LazyAttrComputation>(LazyAttrComputation{
        .compute = memo<ResolvedAttr>(std::move(compute)),
    });
}

struct GitInputScheme : InputScheme
{
    std::optional<Input> inputFromURL(const Settings & settings, const ParsedURL & url, bool requireTree) const override
    {
        if (url.scheme != "git" && parseUrlScheme(url.scheme).application != "git")
            return {};

        auto url2(url);
        url2.query.clear();

        Attrs attrs;
        attrs.emplace("type", "git");

        for (auto & [name, value] : url.query) {
            if (name == "rev" || name == "ref" || name == "keytype" || name == "publicKey" || name == "publicKeys")
                attrs.emplace(name, value);
            else if (
                name == "shallow" || name == "submodules" || name == "lfs" || name == "exportIgnore"
                || name == "allRefs" || name == "verifyCommit")
                attrs.emplace(name, Explicit<bool>{value == "1"});
            else
                url2.query.emplace(name, value);
        }

        attrs.emplace("url", url2.to_string());

        return inputFromAttrs(settings, attrs);
    }

    std::string_view schemeName() const override
    {
        return "git";
    }

    std::string schemeDescription() const override
    {
        return stripIndentation(R"(
          Fetch a Git tree and copy it to the Nix store.
          This is similar to [`builtins.fetchGit`](@docroot@/language/builtins.md#builtins-fetchGit).
        )");
    }

    const std::map<std::string, AttributeInfo> & allowedAttrs() const override
    {
        static const std::map<std::string, AttributeInfo> attrs = {
            {
                "url",
                {
                    .type = "String",
                    .required = true,
                    .doc = R"(
                      The URL formats supported are the same as for Git itself.

                      > **Example**
                      >
                      > ```nix
                      > fetchTree {
                      >   type = "git";
                      >   url = "git@github.com:NixOS/nixpkgs.git";
                      > }
                      > ```

                      > **Note**
                      >
                      > If the URL points to a local directory, and no `ref` or `rev` is given, Nix only considers files added to the Git index, as listed by `git ls-files` but uses the *current file contents* of the Git working directory.
                    )",
                },
            },
            {
                "ref",
                {
                    .type = "String",
                    .required = false,
                    .doc = R"(
                      By default, this has no effect. This becomes relevant only once `shallow` cloning is disabled.

                      A [Git reference](https://git-scm.com/book/en/v2/Git-Internals-Git-References), such as a branch or tag name.

                      Default: `"HEAD"`
                    )",
                },
            },
            {
                "rev",
                {
                    .type = "String",
                    .required = false,
                    .doc = R"(
                      A Git revision; a commit hash.

                      Default: the tip of `ref`
                    )",
                },
            },
            {
                "shallow",
                {
                    .type = "Bool",
                    .required = false,
                    .doc = R"(
                      Make a shallow clone when fetching the Git tree.
                      When this is enabled, the options `ref` and `allRefs` have no effect anymore.

                      Default: `true`
                    )",
                },
            },
            {
                "submodules",
                {
                    .type = "Bool",
                    .required = false,
                    .doc = R"(
                      Also fetch submodules if available.

                      Default: `false`
                    )",
                },
            },
            {
                "lfs",
                {
                    .type = "Bool",
                    .required = false,
                    .doc = R"(
                      Fetch any [Git LFS](https://git-lfs.com/) files.

                      Default: `false`
                    )",
                },
            },
            {
                "exportIgnore",
                {},
            },
            {
                "lastModified",
                {
                    .type = "Integer",
                    .required = false,
                    .doc = R"(
                      Unix timestamp of the fetched commit.

                      If set, pass through the value to the output attribute set.
                      Otherwise, generated from the fetched Git tree.
                    )",
                },
            },
            {
                "revCount",
                {
                    .type = "Integer",
                    .required = false,
                    .doc = R"(
                      Number of revisions in the history of the Git repository before the fetched commit.

                      If set, pass through the value to the output attribute set.
                      Otherwise, generated from the fetched Git tree.
                    )",
                },
            },
            {
                "narHash",
                {},
            },
            {
                "allRefs",
                {
                    .type = "Bool",
                    .required = false,
                    .doc = R"(
                      By default, this has no effect. This becomes relevant only once `shallow` cloning is disabled.

                      Whether to fetch all references (eg. branches and tags) of the repository.
                      With this argument being true, it's possible to load a `rev` from *any* `ref`.
                      (Without setting this option, only `rev`s from the specified `ref` are supported).

                      Default: `false`
                    )",
                },
            },
            {
                "name",
                {},
            },
            {
                "dirtyRev",
                {},
            },
            {
                "dirtyShortRev",
                {},
            },
            {
                "verifyCommit",
                {},
            },
            {
                "keytype",
                {},
            },
            {
                "publicKey",
                {},
            },
            {
                "publicKeys",
                {},
            },
        };
        return attrs;
    }

    std::optional<Input> inputFromAttrs(const Settings & settings, const Attrs & attrs) const override
    {
        for (auto & [name, _] : attrs)
            if (name == "verifyCommit" || name == "keytype" || name == "publicKey" || name == "publicKeys")
                experimentalFeatureSettings.require(Xp::VerifiedFetches);

        maybeGetBoolAttr(attrs, "verifyCommit");

        if (auto ref = maybeGetStrAttr(attrs, "ref"); ref && !isLegalRefName(*ref))
            throw BadURL("invalid Git branch/tag name '%s'", *ref);

        Input input{};
        input.attrs = attrs;
        input.attrs["url"] = fixGitURL(getStrAttr(attrs, "url")).to_string();
        getShallowAttr(input);
        getSubmodulesAttr(input);
        getAllRefsAttr(input);
        return input;
    }

    ParsedURL toURL(const Input & input) const override
    {
        auto url = parseURL(getStrAttr(input.attrs, "url"));
        if (url.scheme != "git")
            url.scheme = "git+" + url.scheme;
        if (auto rev = input.getRev())
            url.query.insert_or_assign("rev", rev->gitRev());
        if (auto ref = input.getRef())
            url.query.insert_or_assign("ref", *ref);
        if (getShallowAttr(input))
            url.query.insert_or_assign("shallow", "1");
        if (getLfsAttr(input))
            url.query.insert_or_assign("lfs", "1");
        if (getSubmodulesAttr(input))
            url.query.insert_or_assign("submodules", "1");
        if (maybeGetBoolAttr(input.attrs, "exportIgnore").value_or(false))
            url.query.insert_or_assign("exportIgnore", "1");
        if (maybeGetBoolAttr(input.attrs, "verifyCommit").value_or(false))
            url.query.insert_or_assign("verifyCommit", "1");
        auto publicKeys = getPublicKeys(input.attrs);
        if (publicKeys.size() == 1) {
            url.query.insert_or_assign("keytype", publicKeys.at(0).type);
            url.query.insert_or_assign("publicKey", publicKeys.at(0).key);
        } else if (publicKeys.size() > 1)
            url.query.insert_or_assign("publicKeys", publicKeys_to_string(publicKeys));
        return url;
    }

    Input applyOverrides(const Input & input, std::optional<std::string> ref, std::optional<Hash> rev) const override
    {
        auto res(input);
        if (rev)
            res.attrs.insert_or_assign("rev", rev->gitRev());
        if (ref)
            res.attrs.insert_or_assign("ref", *ref);
        if (!res.getRef() && res.getRev())
            throw Error("Git input '%s' has a commit hash but no branch/tag name", res.to_string());
        return res;
    }

    void clone(const Settings & settings, Store & store, const Input & input, const std::filesystem::path & destDir)
        const override
    {
        auto repoInfo = getRepoInfo(input);

        OsStrings args = {OS_STR("clone")};

        args.push_back(string_to_os_string(repoInfo.locationToArg()));

        if (auto ref = input.getRef()) {
            args.push_back(OS_STR("--branch"));
            args.push_back(string_to_os_string(*ref));
        }

        if (input.getRev())
            throw UnimplementedError("cloning a specific revision is not implemented");

        args.push_back(destDir.native());

        runProgram("git", true, args, true);
    }

    std::optional<std::filesystem::path> getSourcePath(const Input & input) const override
    {
        return getRepoInfo(input).getPath();
    }

    void putFile(
        const Input & input,
        const CanonPath & path,
        std::string_view contents,
        std::optional<std::string> commitMsg) const override
    {
        auto repoInfo = getRepoInfo(input);
        auto repoPath = repoInfo.getPath();
        if (!repoPath)
            throw Error(
                "cannot commit '%s' to Git repository '%s' because it's not a working tree", path, input.to_string());

        writeFile(*repoPath / path.rel(), contents);

        auto result = runProgram(
            RunOptions{
                .program = "git",
                .args{
                    OS_STR("-C"),
                    repoPath->native(),
                    OS_STR("--git-dir"),
                    string_to_os_string(repoInfo.gitDir),
                    OS_STR("check-ignore"),
                    OS_STR("--quiet"),
                    string_to_os_string(std::string(path.rel())),
                },
            });
        auto exitCode =
#ifndef WIN32 // TODO abstract over exit status handling on Windows
            WEXITSTATUS(result.first)
#else
            result.first
#endif
            ;

        if (exitCode != 0) {
            // The path is not `.gitignore`d, we can add the file.
            runProgram(
                "git",
                true,
                {
                    OS_STR("-C"),
                    repoPath->native(),
                    OS_STR("--git-dir"),
                    string_to_os_string(repoInfo.gitDir),
                    OS_STR("add"),
                    OS_STR("--intent-to-add"),
                    OS_STR("--"),
                    string_to_os_string(std::string(path.rel())),
                });

            if (commitMsg) {
                auto [tempFd, tempPath] = createTempFile("nix-msg");
                AutoDelete delTemp(tempPath, /*recursive=*/false);
                writeFull(tempFd.get(), *commitMsg);

                // Pause the logger to allow for user input (such as a gpg passphrase) in `git commit`
                auto suspension = logger->suspend();
                runProgram(
                    "git",
                    true,
                    {
                        OS_STR("-C"),
                        repoPath->native(),
                        OS_STR("--git-dir"),
                        string_to_os_string(repoInfo.gitDir),
                        OS_STR("commit"),
                        string_to_os_string(std::string(path.rel())),
                        OS_STR("-F"),
                        tempPath.native(),
                    });

                delTemp.deletePath();
            }
        }
    }

    struct RepoInfo
    {
        /* Either the path of the repo (for local, non-bare repos), or
           the URL (which is never a `file` URL). */
        std::variant<std::filesystem::path, ParsedURL> location;

        /* Working directory info: the complete list of files, and
           whether the working directory is dirty compared to HEAD. */
        GitRepo::WorkdirInfo workdirInfo;

        std::string locationToArg() const
        {
            return std::visit(
                overloaded{
                    [&](const std::filesystem::path & path) { return path.string(); },
                    [&](const ParsedURL & url) { return url.to_string(); }},
                location);
        }

        std::optional<std::filesystem::path> getPath() const
        {
            if (auto path = std::get_if<std::filesystem::path>(&location))
                return *path;
            else
                return std::nullopt;
        }

        void warnDirty(const Settings & settings) const
        {
            if (workdirInfo.isDirty) {
                if (!settings.allowDirty)
                    throw Error("Git tree '%s' is dirty", locationToArg());

                if (settings.warnDirty)
                    warn("Git tree '%s' is dirty", locationToArg());
            }
        }

        std::string gitDir = ".git";
    };

    bool getShallowAttr(const Input & input) const
    {
        return maybeGetBoolAttr(input.attrs, "shallow").value_or(false);
    }

    bool getSubmodulesAttr(const Input & input) const
    {
        return maybeGetBoolAttr(input.attrs, "submodules").value_or(false);
    }

    bool getLfsAttr(const Input & input) const
    {
        return maybeGetBoolAttr(input.attrs, "lfs").value_or(false);
    }

    bool getExportIgnoreAttr(const Input & input) const
    {
        return maybeGetBoolAttr(input.attrs, "exportIgnore").value_or(false);
    }

    bool getAllRefsAttr(const Input & input) const
    {
        return maybeGetBoolAttr(input.attrs, "allRefs").value_or(false);
    }

    RepoInfo getRepoInfo(const Input & input) const
    {
        auto checkHashAlgorithm = [&](const std::optional<Hash> & hash) {
            if (hash.has_value() && !(hash->algo == HashAlgorithm::SHA1 || hash->algo == HashAlgorithm::SHA256))
                throw Error(
                    "Hash '%s' is not supported by Git. Supported types are sha1 and sha256.",
                    hash->to_string(HashFormat::Base16, true));
        };

        if (auto rev = input.getRev())
            checkHashAlgorithm(rev);

        RepoInfo repoInfo;

        // file:// URIs are normally not cloned (but otherwise treated the
        // same as remote URIs, i.e. we don't use the working tree or
        // HEAD). Exception: If _NIX_FORCE_HTTP is set, or the repo is a bare git
        // repo, treat as a remote URI to force a clone.
        static bool forceHttp = getEnv("_NIX_FORCE_HTTP") == "1"; // for testing
        auto url = parseURL(getStrAttr(input.attrs, "url"));

        // Why are we checking for bare repository?
        // well if it's a bare repository we want to force a git fetch rather than copying the folder
        auto isBareRepository = [](const std::filesystem::path & path) {
            return pathExists(path) && !pathExists(path / ".git");
        };

        // FIXME: here we turn a possibly relative path into an absolute path.
        // This allows relative git flake inputs to be resolved against the
        // **current working directory** (as in POSIX), which tends to work out
        // ok in the context of flakes, but is the wrong behavior,
        // as it should resolve against the flake.nix base directory instead.
        //
        // See: https://discourse.nixos.org/t/57783 and #9708
        //
        auto maybeUrlFsPathForFileUrl =
            url.scheme == "file" ? std::make_optional(urlPathToPath(url.path)) : std::nullopt;
        if (maybeUrlFsPathForFileUrl && !forceHttp && !isBareRepository(*maybeUrlFsPathForFileUrl)) {
            auto & path = *maybeUrlFsPathForFileUrl;

            if (!path.is_absolute()) {
                warn(
                    "Fetching Git repository '%s', which uses a path relative to the current directory. "
                    "This is not supported and will stop working in a future release. "
                    "See https://github.com/NixOS/nix/issues/12281 for details.",
                    url);
            }

            repoInfo.location = std::filesystem::absolute(path);
        } else {
            if (maybeUrlFsPathForFileUrl)
                /* Query parameters are meaningless for file://, but
                   Git interprets them as part of the file name. So get
                   rid of them. */
                url.query.clear();
            /* Backward compatibility hack: In old versions of Nix, if you had
               a flake input like

                 inputs.foo.url = "git+https://foo/bar?dir=subdir";

               it would result in a lock file entry like

                 "original": {
                   "dir": "subdir",
                   "type": "git",
                   "url": "https://foo/bar?dir=subdir"
                 }

               New versions of Nix remove `?dir=subdir` from the `url` field,
               since the subdirectory is intended for `FlakeRef`, not the
               fetcher (and specifically the remote server), that is, the
               flakeref is parsed into

                 "original": {
                   "dir": "subdir",
                   "type": "git",
                   "url": "https://foo/bar"
                 }

               However, new versions of nix parsing old flake.lock files would pass the dir=
               query parameter in the "url" attribute to git, which will then complain.

               For this reason, we are filtering the `dir` query parameter from the URL
               before passing it to git. */
            url.query.erase("dir");
            repoInfo.location = url;
        }

        // If this is a local directory and no ref or revision is
        // given, then allow the use of an unclean working tree.
        if (auto repoPath = repoInfo.getPath(); !input.getRef() && !input.getRev() && repoPath)
            repoInfo.workdirInfo = GitRepo::getCachedWorkdirInfo(*repoPath);

        return repoInfo;
    }

    uint64_t getLastModified(
        const Settings & settings,
        const RepoInfo & repoInfo,
        const std::filesystem::path & repoDir,
        const Hash & rev) const
    {
        return GitLastModified::lookup(
            settings, rev, [&] { return GitRepo::openRepo(repoDir, {})->getLastModified(rev); });
    }

    uint64_t getRevCount(
        const Settings & settings,
        const RepoInfo & repoInfo,
        const std::filesystem::path & repoDir,
        const Hash & rev) const
    {
        return GitRevCount::lookup(settings, rev, [&] {
            if (GitRepo::openRepo(repoDir, {})->isShallow())
                throw Error(
                    "'%s' is a shallow Git repository, so 'revCount' is not available", repoInfo.locationToArg());
            Activity act(
                *logger, lvlChatty, actUnknown, fmt("getting Git revision count of '%s'", repoInfo.locationToArg()));
            return GitRepo::openRepo(repoDir, {})->getRevCount(rev);
        });
    }

    LazyAttr lazyRevCount(
        const Settings & settings,
        const RepoInfo & repoInfo,
        const std::filesystem::path & repoDir,
        const Hash & rev) const
    {
        return makeLazyAttr([this, &settings, repoInfo, repoDir, rev]() -> ResolvedAttr {
            return getRevCount(settings, repoInfo, repoDir, rev);
        });
    }

    std::string getDefaultRef(const Settings & settings, const RepoInfo & repoInfo, bool shallow) const
    {
        auto head = std::visit(
            overloaded{
                [&](const std::filesystem::path & path) { return GitRepo::openRepo(path, {})->getWorkdirRef(); },
                [&](const ParsedURL & url) { return readHeadCached(settings, url.to_string(), shallow); }},
            repoInfo.location);
        if (!head) {
            warn("could not read HEAD ref from repo at '%s', using 'master'", repoInfo.locationToArg());
            return "master";
        }
        return *head;
    }

    static MakeNotAllowedError makeNotAllowedError(std::filesystem::path repoPath)
    {
        return [repoPath{std::move(repoPath)}](const CanonPath & path) -> RestrictedPathError {
            if (pathExists(repoPath / path.rel()))
                return RestrictedPathError(
                    "Path '%1%' in the repository %2% is not tracked by Git.\n"
                    "\n"
                    "To make it visible to Nix, run:\n"
                    "\n"
                    "git -C %2% add \"%1%\"",
                    path.rel(),
                    PathFmt(repoPath));
            else
                return RestrictedPathError(
                    "Path '%s' does not exist in Git repository %s.", path.rel(), PathFmt(repoPath));
        };
    }

    void verifyCommit(const Input & input, std::shared_ptr<GitRepo> repo) const
    {
        auto publicKeys = getPublicKeys(input.attrs);
        auto verifyCommit = maybeGetBoolAttr(input.attrs, "verifyCommit").value_or(!publicKeys.empty());

        if (verifyCommit) {
            if (input.getRev() && repo)
                repo->verifyCommit(*input.getRev(), publicKeys);
            else
                throw Error(
                    "commit verification is required for Git repository '%s', but it's dirty", input.to_string());
        }
    }

    std::pair<ref<SourceAccessor>, Input>
    getAccessorFromCommit(const Settings & settings, Store & store, RepoInfo & repoInfo, Input && input) const
    {
        assert(!repoInfo.workdirInfo.isDirty);

        auto origRev = input.getRev();

        auto originalRef = input.getRef();
        bool shallow = getShallowAttr(input);
        auto ref = originalRef ? *originalRef : getDefaultRef(settings, repoInfo, shallow);
        input.attrs.insert_or_assign("ref", ref);

        std::filesystem::path repoDir;

        if (auto repoPath = repoInfo.getPath()) {
            repoDir = *repoPath;
            if (!input.getRev())
                input.attrs.insert_or_assign("rev", GitRepo::openRepo(repoDir, {})->resolveRef(ref).gitRev());
        } else {
            auto repoUrl = std::get<ParsedURL>(repoInfo.location);
            std::filesystem::path cacheDir = getCachePath(repoUrl.to_string(), shallow);
            repoDir = cacheDir;
            repoInfo.gitDir = ".";

            createDirs(cacheDir.parent_path());
            PathLocks cacheDirLock({cacheDir.string()});

            auto repo = GitRepo::openRepo(cacheDir, {.create = true, .bare = true});

            // We need to set the origin so resolving submodule URLs works
            repo->setRemote("origin", repoUrl.to_string());

            auto localRefFile = ref.compare(0, 5, "refs/") == 0 ? cacheDir / ref : cacheDir / "refs/heads" / ref;

            bool doFetch = false;
            time_t now = time(nullptr);

            /* If a rev was specified, we need to fetch if it's not in the
               repo. */
            if (auto rev = input.getRev()) {
                doFetch = !repo->hasObject(*rev);
            } else {
                if (getAllRefsAttr(input)) {
                    doFetch = true;
                } else {
                    /* If the local ref is older than 'tarball-ttl' seconds, do a
                       git fetch to update the local ref to the remote ref. */
                    auto st = maybeStat(localRefFile);
                    doFetch = !st || !isCacheFileWithinTtl(settings, now, *st);
                }
            }

            if (doFetch) {
                bool shallow = getShallowAttr(input);

                /* Decide whether to fetch this remote as a partial
                   (blob-less) clone so blobs are backfilled on demand
                   (the `git-lazy-fetch` setting / `NIX_GIT_LAZY_FETCH`).
                   Gate on the SAME condition the on-demand backfill
                   below (the `GitPromisorProvider`) requires: an
                   http(s)- or ssh-transported remote advertising
                   protocol-v2 with the `filter` capability. Marking a
                   remote we can't actually backfill from (e.g.
                   `file://`, `git://`, or a filter-less server) would
                   route a later missing-blob read to a provider that
                   throws, so we only mark when the provider will
                   succeed. For every other remote we fall through to a
                   normal full fetch.

                   The `gitV2CapsCache` probe is a single cached
                   round-trip per URL (30-min TTL), transport-matched to
                   the URL scheme (an HTTP GET for http(s); an ssh
                   `git-upload-pack` advertisement read for ssh) and
                   shared with the provider-attach probe in
                   `getAccessor` below, so this adds no extra network
                   cost in the common case. */
                bool lazyFetch = false;
                if (settings.gitLazyFetch
                    && (repoUrl.scheme == "https" || repoUrl.scheme == "http" || repoUrl.scheme == "ssh")) {
                    if (gitV2CapsCache(repoUrl.to_string()).supportsFilter) {
                        repo->markAsPartialClone("origin");
                        lazyFetch = true;
                        debug(
                            "git-lazy-fetch: remote '%s' supports filtered fetch — fetching as a partial clone "
                            "(blobs on demand)",
                            repoUrl.to_string());
                    } else {
                        debug(
                            "git-lazy-fetch: remote '%s' does not advertise protocol-v2 'filter'; doing a full fetch",
                            repoUrl.to_string());
                    }
                }

                try {
                    auto fetchRef = getAllRefsAttr(input)             ? "refs/*:refs/*"
                                    : input.getRev()                  ? input.getRev()->gitRev()
                                    : ref.compare(0, 5, "refs/") == 0 ? fmt("%1%:%1%", ref)
                                    : ref == "HEAD"                   ? "HEAD:HEAD"
                                                                      : fmt("%1%:%1%", "refs/heads/" + ref);

                    repo->fetch(repoUrl.to_string(), fetchRef, shallow, lazyFetch);
                } catch (Error & e) {
                    if (!std::filesystem::exists(localRefFile))
                        throw;
                    logError(e.info());
                    warn(
                        "could not update local clone of Git repository '%s'; continuing with the most recent version",
                        repoInfo.locationToArg());
                }

                try {
                    if (!input.getRev())
                        setWriteTime(localRefFile, now, now);
                } catch (Error & e) {
                    warn("could not update mtime for file %s: %s", PathFmt(localRefFile), e.info().msg);
                }
                if (!originalRef && !storeCachedHead(repoUrl.to_string(), shallow, ref))
                    warn("could not update cached head '%s' for '%s'", ref, repoInfo.locationToArg());
            }

            if (auto rev = input.getRev()) {
                if (!repo->hasObject(*rev))
                    throw Error(
                        "Cannot find Git revision '%s' in ref '%s' of repository '%s'! "
                        "Please make sure that the " ANSI_BOLD "rev" ANSI_NORMAL " exists on the " ANSI_BOLD
                        "ref" ANSI_NORMAL " you've specified or add " ANSI_BOLD "allRefs = true;" ANSI_NORMAL
                        " to " ANSI_BOLD "fetchGit" ANSI_NORMAL ".",
                        rev->gitRev(),
                        ref,
                        repoInfo.locationToArg());
            } else
                input.attrs.insert_or_assign("rev", repo->resolveRef(ref).gitRev());

            // cache dir lock is removed at scope end; we will only use read-only operations on specific revisions in
            // the remainder
        }

        auto repo = GitRepo::openRepo(repoDir, {});

        // FIXME: check whether rev is an ancestor of ref?

        auto rev = *input.getRev();

        /* Skip lastModified computation if it's already supplied by the caller.
           We don't care if they specify an incorrect value; it doesn't
           matter for security, unlike narHash. */
        if (!input.attrs.contains("lastModified"))
            input.attrs.insert_or_assign("lastModified", getLastModified(settings, repoInfo, repoDir, rev));

        if (!getShallowAttr(input)) {
            /* Like lastModified, skip revCount if supplied by the caller. */
            if (!input.attrs.contains("revCount"))
                input.attrs.insert_or_assign("revCount", lazyRevCount(settings, repoInfo, repoDir, rev));
        }

        printTalkative("using revision %s of repo '%s'", rev.gitRev(), repoInfo.locationToArg());

        verifyCommit(input, repo);

        bool exportIgnore = getExportIgnoreAttr(input);
        bool smudgeLfs = getLfsAttr(input);

        /* If this is a partial clone (`extensions.partialClone` names a
           remote in the repo's git config), build a `GitPromisorProvider`
           against that remote so missing blobs can be fetched on
           demand. We only attach the provider if the remote actually
           supports protocol-v2 with `filter` capability — otherwise
           `ensureObjects` would throw. The capability probe is cached
           per-URL with a 30-minute TTL. */
        std::shared_ptr<GitPromisorProvider> provider;
        if (auto remoteUrl = repo->getPartialCloneRemoteUrl()) {
            /* This repo IS already a partial clone (the remote is marked
               as a promisor), so it WILL have missing blobs that only the
               provider can backfill. `makeGitPromisorProvider` may throw
               on an unsupported transport scheme (e.g. an externally
               created `git://`/`file://` partial clone) — catch that and
               leave `provider` null with a clear warning rather than
               aborting the fetch. */
            try {
                provider = makeGitPromisorProvider(*remoteUrl, repoDir);
            } catch (Error & e) {
                warn(
                    "cannot attach a lazy-fetch provider to partial-clone remote '%s' (%s); "
                    "missing blobs will not be fetched on demand",
                    *remoteUrl,
                    e.what());
            }
            if (provider) {
                auto caps = gitV2CapsCache(*remoteUrl);
                /* Detach ONLY on an authoritative "no filter" answer. On a
                   transient probe failure keep the provider: per-blob
                   on-demand backfill (each read re-attempts and surfaces a
                   clear error if the remote truly can't serve it) is
                   strictly better than no provider on a clone that is
                   already missing blobs. */
                if (!caps.supportsFilter && !caps.probeFailed) {
                    debug(
                        "partial-clone remote '%s' does not advertise protocol v2 with fetch filters; "
                        "missing blobs will not be fetched on demand",
                        *remoteUrl);
                    provider.reset();
                } else if (caps.probeFailed) {
                    debug(
                        "partial-clone capability probe for '%s' failed transiently; keeping the backfill provider "
                        "(per-blob on-demand fetch)",
                        *remoteUrl);
                }
            }
        }

        auto accessor = repo->getAccessor(
            rev,
            {.exportIgnore = exportIgnore, .smudgeLfs = smudgeLfs, .provider = provider},
            "«" + input.to_string() + "»");

        /* If the repo has submodules, fetch them and return a mounted
           input accessor consisting of the accessor for the top-level
           repo and the accessors for the submodules. */
        if (getSubmodulesAttr(input)) {
            std::map<CanonPath, nix::ref<SourceAccessor>> mounts;

            for (auto & [submodule, submoduleRev] : repo->getSubmodules(rev, exportIgnore)) {
                auto resolved = repo->resolveSubmoduleUrl(submodule.url);
                debug(
                    "Git submodule %s: %s %s %s -> %s",
                    submodule.path,
                    submodule.url,
                    submodule.branch,
                    submoduleRev.gitRev(),
                    resolved);
                fetchers::Attrs attrs;
                attrs.insert_or_assign("type", "git");
                attrs.insert_or_assign("url", resolved);
                if (submodule.branch != "") {
                    // A special value of . is used to indicate that the name of the branch in the submodule
                    // should be the same name as the current branch in the current repository.
                    // https://git-scm.com/docs/gitmodules
                    if (submodule.branch == ".") {
                        attrs.insert_or_assign("ref", ref);
                    } else {
                        attrs.insert_or_assign("ref", submodule.branch);
                    }
                }
                attrs.insert_or_assign("rev", submoduleRev.gitRev());
                attrs.insert_or_assign("exportIgnore", Explicit<bool>{exportIgnore});
                attrs.insert_or_assign("submodules", Explicit<bool>{true});
                attrs.insert_or_assign("lfs", Explicit<bool>{smudgeLfs});
                attrs.insert_or_assign("allRefs", Explicit<bool>{true});
                auto submoduleInput = fetchers::Input::fromAttrs(settings, std::move(attrs));
                auto [submoduleAccessor, submoduleInput2] = submoduleInput.getAccessor(settings, store);
                submoduleAccessor->setPathDisplay("«" + submoduleInput.to_string() + "»");
                mounts.insert_or_assign(submodule.path, submoduleAccessor);
            }

            if (!mounts.empty()) {
                mounts.insert_or_assign(CanonPath::root, accessor);
                /* The submodule mount set is fixed at construction (no
                   runtime `mount()` calls follow), so use the immutable
                   `Switch` rather than the mutable `Mounted`. */
                accessor = makeSwitch(std::move(mounts));
            }
        }

        assert(!origRev || origRev == rev);

        return {accessor, std::move(input)};
    }

    std::pair<ref<SourceAccessor>, Input>
    getAccessorFromWorkdir(const Settings & settings, Store & store, RepoInfo & repoInfo, Input && input) const
    {
        auto repoPath = repoInfo.getPath().value();

        if (getSubmodulesAttr(input))
            /* Create mountpoints for the submodules. */
            for (auto & submodule : repoInfo.workdirInfo.submodules)
                repoInfo.workdirInfo.files.insert(submodule.path);

        auto repo = GitRepo::openRepo(repoPath, {});

        auto exportIgnore = getExportIgnoreAttr(input);

        ref<SourceAccessor> diskAccessor =
            repo->getAccessor(repoInfo.workdirInfo, {.exportIgnore = exportIgnore}, makeNotAllowedError(repoPath));

        /* If the workdir is dirty AND the repo has a base commit,
           wrap the disk accessor in a SourceView Overlay rooted at
           the committed tree. The overlay's base is the committed
           tree (content-addressed, cacheable); reads of dirty paths
           route to the disk accessor; deletions become whiteouts.
           Soundness: the overlay's getFingerprint returns nullopt
           (or the wrapper's Input-level fingerprint when set) for
           any path within overlay reach, so cache rows keyed on
           `tree:<headRev>` don't hit for dirty content.

           Clean (`!isDirty`) and no-headRev cases bypass the overlay
           — the disk accessor flows through unchanged. */
        ref<SourceAccessor> accessor = diskAccessor;
        if (repoInfo.workdirInfo.isDirty && repoInfo.workdirInfo.headRev) {
            try {
                auto baseAccessor = repo->getAccessor(
                    *repoInfo.workdirInfo.headRev,
                    {.exportIgnore = exportIgnore},
                    "«" + input.to_string() + " @ " + repoInfo.workdirInfo.headRev->gitShortRev() + "»");
                accessor = makeWorkdirOverlay(baseAccessor, diskAccessor, repoInfo.workdirInfo).cast<SourceAccessor>();
            } catch (Error & e) {
                /* If the committed tree can't be read (corrupt repo
                   or missing object), fall back to the flat path
                   rather than fail eval. Same bytes, just no
                   cache-row reuse for clean siblings. */
                debug(
                    "workdir-overlay failed for '%s'; falling back to flat workdir accessor: %s",
                    repoPath.string(),
                    e.what());
            }
        }

        /* If the repo has submodules, return a mounted input accessor
           consisting of the accessor for the top-level repo and the
           accessors for the submodule workdirs. */
        if (getSubmodulesAttr(input) && !repoInfo.workdirInfo.submodules.empty()) {
            std::map<CanonPath, nix::ref<SourceAccessor>> mounts;

            for (auto & submodule : repoInfo.workdirInfo.submodules) {
                auto submodulePath = repoPath / submodule.path.rel();
                fetchers::Attrs attrs;
                attrs.insert_or_assign("type", "git");
                attrs.insert_or_assign("url", submodulePath.string());
                attrs.insert_or_assign("exportIgnore", Explicit<bool>{exportIgnore});
                attrs.insert_or_assign("submodules", Explicit<bool>{true});
                // TODO: fall back to getAccessorFromCommit-like fetch when submodules aren't checked out
                // attrs.insert_or_assign("allRefs", Explicit<bool>{ true });

                auto submoduleInput = fetchers::Input::fromAttrs(settings, std::move(attrs));
                auto [submoduleAccessor, submoduleInput2] = submoduleInput.getAccessor(settings, store);
                submoduleAccessor->setPathDisplay("«" + submoduleInput.to_string() + "»");

                /* If the submodule is dirty, mark this repo dirty as
                   well. */
                if (!submoduleInput2.getRev())
                    repoInfo.workdirInfo.isDirty = true;

                mounts.insert_or_assign(submodule.path, submoduleAccessor);
            }

            mounts.insert_or_assign(CanonPath::root, accessor);
            /* Fixed mount set (no runtime `mount()` follows) — immutable
               `Switch`, not the mutable `Mounted`. */
            accessor = makeSwitch(std::move(mounts));
        }

        if (!repoInfo.workdirInfo.isDirty) {
            auto repo = GitRepo::openRepo(repoPath, {});

            if (auto ref = repo->getWorkdirRef())
                input.attrs.insert_or_assign("ref", *ref);

            /* Return a rev of 000... if there are no commits yet. */
            auto rev = repoInfo.workdirInfo.headRev.value_or(nullRev);

            input.attrs.insert_or_assign("rev", rev.gitRev());
            if (!getShallowAttr(input)) {
                if (rev == nullRev) {
                    input.attrs.insert_or_assign("revCount", uint64_t(0));
                } else {
                    input.attrs.insert_or_assign("revCount", lazyRevCount(settings, repoInfo, repoPath, rev));
                }
            }

            verifyCommit(input, repo);
        } else {
            repoInfo.warnDirty(settings);

            if (repoInfo.workdirInfo.headRev) {
                input.attrs.insert_or_assign("dirtyRev", repoInfo.workdirInfo.headRev->gitRev() + "-dirty");
                input.attrs.insert_or_assign("dirtyShortRev", repoInfo.workdirInfo.headRev->gitShortRev() + "-dirty");
            }

            verifyCommit(input, nullptr);
        }

        input.attrs.insert_or_assign(
            "lastModified",
            repoInfo.workdirInfo.headRev ? getLastModified(settings, repoInfo, repoPath, *repoInfo.workdirInfo.headRev)
                                         : 0);

        return {accessor, std::move(input)};
    }

    std::pair<ref<SourceAccessor>, Input>
    getAccessor(const Settings & settings, Store & store, const Input & _input) const override
    {
        Input input(_input);

        auto repoInfo = getRepoInfo(input);

        if (getExportIgnoreAttr(input) && getSubmodulesAttr(input)) {
            /* In this situation, we don't have a git CLI behavior that we can copy.
               `git archive` does not support submodules, so it is unclear whether
               rules from the parent should affect the submodule or not.
               When git may eventually implement this, we need Nix to match its
               behavior. */
            throw UnimplementedError("exportIgnore and submodules are not supported together yet");
        }

        auto [accessor, final] = input.getRef() || input.getRev() || !repoInfo.getPath()
                                     ? getAccessorFromCommit(settings, store, repoInfo, std::move(input))
                                     : getAccessorFromWorkdir(settings, store, repoInfo, std::move(input));

        return {accessor, std::move(final)};
    }

    std::optional<std::string> getFingerprint(Store & store, const Input & input) const override
    {
        auto makeFingerprint = [&](const Hash & rev) {
            return rev.gitRev() + (getSubmodulesAttr(input) ? ";s" : "") + (getExportIgnoreAttr(input) ? ";e" : "")
                   + (getLfsAttr(input) ? ";l" : "");
        };

        if (auto rev = input.getRev())
            return makeFingerprint(*rev);
        else {
            auto repoInfo = getRepoInfo(input);
            if (auto repoPath = repoInfo.getPath(); repoPath && repoInfo.workdirInfo.submodules.empty()) {
                /* Calculate a fingerprint that takes into account the
                   deleted and modified/added files. */
                HashSink hashSink{HashAlgorithm::SHA512};
                for (auto & file : repoInfo.workdirInfo.dirtyFiles) {
                    writeString("modified:", hashSink);
                    writeString(file.abs(), hashSink);
                    dumpPath(*repoPath / file.rel(), hashSink);
                }
                for (auto & file : repoInfo.workdirInfo.deletedFiles) {
                    writeString("deleted:", hashSink);
                    writeString(file.abs(), hashSink);
                }
                return makeFingerprint(repoInfo.workdirInfo.headRev.value_or(nullRev))
                       + ";d=" + hashSink.finish().hash.to_string(HashFormat::Base16, false);
            }
            return std::nullopt;
        }
    }

    bool isLocked(const Settings & settings, const Input & input) const override
    {
        auto rev = input.getRev();
        return rev && rev != nullRev;
    }
};

static auto rGitInputScheme = OnStartup([] { registerInputScheme(std::make_unique<GitInputScheme>()); });

} // namespace nix::fetchers
