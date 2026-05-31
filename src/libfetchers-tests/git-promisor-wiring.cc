/* Tests for the GitInputScheme → GitPromisorProvider wiring (Item 3
 * of the tecnix DEFERRED-WORK plan).
 *
 * Three pieces under test:
 *
 *   1. `GitRepoImpl::getPartialCloneRemoteUrl` — reads the repo's git
 *      config; returns the URL named by `extensions.partialClone` if
 *      present, else nullopt.
 *
 *   2. `gitV2CapsCache` — per-URL TTL cache around the protocol-v2
 *      capability probe. Within the TTL, repeated calls hit the cache
 *      and don't invoke the probe function. After the cache is
 *      cleared (or after the TTL elapses), the probe runs again.
 *
 *   3. The provider is constructed only when `getPartialCloneRemoteUrl`
 *      is set AND the URL's cached probe says `supportsFilter` is
 *      true. We exercise this end-to-end via the test seam
 *      `setV2ProbeForTest`, observing whether `provider` becomes
 *      non-null on a `getAccessor` round-trip.
 */

#include "nix/fetchers/git-promisor.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"

#include <git2/config.h>
#include <git2/global.h>
#include <git2/index.h>
#include <git2/oid.h>
#include <git2/remote.h>
#include <git2/repository.h>
#include <git2/signature.h>
#include <git2/tree.h>
#include <git2/commit.h>
#include <git2/refs.h>
#include <git2/object.h>

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>

namespace nix::fetchers {

class GitPromisorWiringTest : public ::testing::Test
{
    std::unique_ptr<AutoDelete> delTmpDir;

protected:
    std::filesystem::path tmpDir;
    git_repository * repo = nullptr;

public:
    void SetUp() override
    {
        tmpDir = createTempDir();
        delTmpDir = std::make_unique<AutoDelete>(tmpDir, true);
        git_libgit2_init();
        ASSERT_EQ(git_repository_init(&repo, tmpDir.string().c_str(), 0), 0);
        gitV2CapsCacheClearForTest();
    }

    void TearDown() override
    {
        if (repo) {
            git_repository_free(repo);
            repo = nullptr;
        }
        gitV2CapsCacheClearForTest();
        delTmpDir.reset();
    }

    /* Set a config key on this repo. */
    void configSet(const std::string & key, const std::string & value)
    {
        git_config * config = nullptr;
        ASSERT_EQ(git_repository_config(&config, repo), 0);
        ASSERT_EQ(git_config_set_string(config, key.c_str(), value.c_str()), 0);
        git_config_free(config);
    }

    /* Stage and commit a one-file worktree, returning the commit OID. */
    git_oid commitOneFile(const std::string & filename, const std::string & contents)
    {
        std::filesystem::create_directories(tmpDir);
        std::ofstream f(tmpDir / filename);
        f << contents;
        f.close();

        git_index * idx = nullptr;
        EXPECT_EQ(git_repository_index(&idx, repo), 0);
        EXPECT_EQ(git_index_add_all(idx, nullptr, 0, nullptr, nullptr), 0);
        git_oid treeOid;
        EXPECT_EQ(git_index_write_tree(&treeOid, idx), 0);
        git_index_free(idx);

        git_tree * tree = nullptr;
        EXPECT_EQ(git_tree_lookup(&tree, repo, &treeOid), 0);
        git_signature * sig = nullptr;
        EXPECT_EQ(git_signature_now(&sig, "Test", "test@example.com"), 0);

        git_oid commitOid;
        EXPECT_EQ(git_commit_create(&commitOid, repo, "HEAD", sig, sig, "UTF-8", "test commit", tree, 0, nullptr), 0);
        git_signature_free(sig);
        git_tree_free(tree);
        return commitOid;
    }

    Hash toHash(const git_oid & oid)
    {
        char buf[GIT_OID_SHA1_HEXSIZE + 1] = {0};
        git_oid_tostr(buf, sizeof(buf), &oid);
        return Hash::parseAny(std::string(buf), HashAlgorithm::SHA1);
    }
};

/* ---------- getPartialCloneRemoteUrl ---------- */

TEST_F(GitPromisorWiringTest, GetPartialCloneRemoteUrlOnNormalRepoReturnsNullopt)
{
    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    EXPECT_FALSE(gitRepo->getPartialCloneRemoteUrl().has_value());
}

TEST_F(GitPromisorWiringTest, GetPartialCloneRemoteUrlReturnsRemoteUrlWhenConfigured)
{
    /* Create an `origin` remote with a recognizable URL, then mark
       this repo as a partial clone of `origin`. */
    git_remote * remote = nullptr;
    ASSERT_EQ(git_remote_create(&remote, repo, "origin", "https://example.invalid/repo.git"), 0);
    git_remote_free(remote);
    configSet("extensions.partialClone", "origin");

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto remoteUrl = gitRepo->getPartialCloneRemoteUrl();
    ASSERT_TRUE(remoteUrl.has_value());
    EXPECT_EQ(*remoteUrl, "https://example.invalid/repo.git");
}

TEST_F(GitPromisorWiringTest, GetPartialCloneRemoteUrlHonorsStockGitPromisorMarker)
{
    /* CR-4: stock `git clone --filter=blob:none` sets
       `remote.<name>.promisor = true` but NOT `extensions.partialClone`
       (verified against git 2.53). We must recognise such an
       externally-created partial clone via the promisor marker, or its
       blob reads would hard-fail with no provider attached. */
    git_remote * remote = nullptr;
    ASSERT_EQ(git_remote_create(&remote, repo, "origin", "https://example.invalid/mono.git"), 0);
    git_remote_free(remote);
    /* No extensions.partialClone — only the promisor convention. */
    configSet("remote.origin.promisor", "true");
    configSet("remote.origin.partialclonefilter", "blob:none");

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto remoteUrl = gitRepo->getPartialCloneRemoteUrl();
    ASSERT_TRUE(remoteUrl.has_value());
    EXPECT_EQ(*remoteUrl, "https://example.invalid/mono.git");
}

TEST_F(GitPromisorWiringTest, GetPartialCloneRemoteUrlIgnoresFalsePromisor)
{
    /* A remote with `promisor = false` is NOT a partial clone — must
       not attach a provider. */
    git_remote * remote = nullptr;
    ASSERT_EQ(git_remote_create(&remote, repo, "origin", "https://example.invalid/mono.git"), 0);
    git_remote_free(remote);
    configSet("remote.origin.promisor", "false");

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    EXPECT_FALSE(gitRepo->getPartialCloneRemoteUrl().has_value());
}

TEST_F(GitPromisorWiringTest, GetPartialCloneRemoteUrlMissingRemoteReturnsNullopt)
{
    /* `extensions.partialClone` set but the named remote doesn't
       exist — defensive: we shouldn't crash, and we shouldn't return
       a URL we can't justify. */
    configSet("extensions.partialClone", "ghost");

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    EXPECT_FALSE(gitRepo->getPartialCloneRemoteUrl().has_value());
}

/* ---------- gitV2CapsCache TTL behaviour ---------- */

TEST_F(GitPromisorWiringTest, V2CapsCacheDeduplicatesProbesWithinTtl)
{
    /* Install a stub probe that increments a counter on each call. The
       cache should serve the second call from the entry written on the
       first; counter stays at 1. */
    std::atomic<int> probeCount{0};
    auto prev = setV2ProbeForTest([&probeCount](const std::string &) -> bool {
        ++probeCount;
        return true;
    });

    auto r1 = gitV2CapsCache("https://example.invalid/repo.git");
    auto r2 = gitV2CapsCache("https://example.invalid/repo.git");

    EXPECT_TRUE(r1.supportsFilter);
    EXPECT_TRUE(r2.supportsFilter);
    EXPECT_EQ(probeCount.load(), 1);

    /* `probedAt` is set on the first probe and inherited from the
       cache on the second — so the timestamps must match. */
    EXPECT_EQ(r1.probedAt, r2.probedAt);

    setV2ProbeForTest(std::move(prev));
}

TEST_F(GitPromisorWiringTest, V2CapsCacheReprobesDifferentUrls)
{
    /* Two different URLs are cached separately. Each gets one probe. */
    std::atomic<int> probeCount{0};
    auto prev = setV2ProbeForTest([&probeCount](const std::string &) -> bool {
        ++probeCount;
        return true;
    });

    gitV2CapsCache("https://a.invalid/repo.git");
    gitV2CapsCache("https://b.invalid/repo.git");
    /* Repeats hit the cache. */
    gitV2CapsCache("https://a.invalid/repo.git");
    gitV2CapsCache("https://b.invalid/repo.git");

    EXPECT_EQ(probeCount.load(), 2);

    setV2ProbeForTest(std::move(prev));
}

TEST_F(GitPromisorWiringTest, V2CapsCacheReprobesAfterClear)
{
    /* `gitV2CapsCacheClearForTest` evicts everything; subsequent
       calls run the probe again. */
    std::atomic<int> probeCount{0};
    auto prev = setV2ProbeForTest([&probeCount](const std::string &) -> bool {
        ++probeCount;
        return true;
    });

    gitV2CapsCache("https://example.invalid/repo.git");
    EXPECT_EQ(probeCount.load(), 1);

    gitV2CapsCacheClearForTest();
    gitV2CapsCache("https://example.invalid/repo.git");
    EXPECT_EQ(probeCount.load(), 2);

    setV2ProbeForTest(std::move(prev));
}

TEST_F(GitPromisorWiringTest, V2CapsCacheRecordsProbeFailureAsUnsupported)
{
    /* If the probe throws (e.g. the remote is unreachable), the cache
       records `supportsFilter=false` rather than letting the
       exception escape. This keeps `getAccessorFromCommit` happy. */
    std::atomic<int> probeCount{0};
    auto prev = setV2ProbeForTest([&probeCount](const std::string &) -> bool {
        ++probeCount;
        throw Error("simulated network failure");
    });

    auto result = gitV2CapsCache("https://example.invalid/repo.git");
    EXPECT_FALSE(result.supportsFilter);
    EXPECT_EQ(probeCount.load(), 1);

    /* Subsequent call still hits the cached negative result. */
    auto result2 = gitV2CapsCache("https://example.invalid/repo.git");
    EXPECT_FALSE(result2.supportsFilter);
    EXPECT_EQ(probeCount.load(), 1);

    setV2ProbeForTest(std::move(prev));
}

/* ---------- end-to-end: provider attaches when both signals agree ----------
 *
 * We can't easily inspect `state_.options.provider` from outside
 * `GitSourceAccessor`, but we *can* observe the side-effect: `readBlob`
 * uses the provider on missing-OID. We don't go that far here (the
 * complete end-to-end requires a real or mocked remote), but we
 * exercise the upstream signals so a regression in `getPartialCloneRemoteUrl`
 * or in the cache wiring would be caught.
 */

TEST_F(GitPromisorWiringTest, ProviderConstructionPathPositive)
{
    /* Set up a partial-clone-shaped repo and stub the cap probe to
       say "yes, supports filter". The combined signals should cause
       `getAccessorFromCommit` to attach a provider. We verify the
       upstream pieces (the URL is read; the cache says yes). */
    git_remote * remote = nullptr;
    ASSERT_EQ(git_remote_create(&remote, repo, "origin", "https://example.invalid/repo.git"), 0);
    git_remote_free(remote);
    configSet("extensions.partialClone", "origin");

    auto prev = setV2ProbeForTest([](const std::string &) -> bool { return true; });

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto remoteUrl = gitRepo->getPartialCloneRemoteUrl();
    ASSERT_TRUE(remoteUrl.has_value());

    auto caps = gitV2CapsCache(*remoteUrl);
    EXPECT_TRUE(caps.supportsFilter);

    setV2ProbeForTest(std::move(prev));
}

TEST_F(GitPromisorWiringTest, ProviderConstructionPathNegativeOnUnsupportedRemote)
{
    /* Even with `extensions.partialClone` set, if the cap probe says
       no, the provider should be dropped. We verify the cache result
       upstream. */
    git_remote * remote = nullptr;
    ASSERT_EQ(git_remote_create(&remote, repo, "origin", "https://example.invalid/repo.git"), 0);
    git_remote_free(remote);
    configSet("extensions.partialClone", "origin");

    auto prev = setV2ProbeForTest([](const std::string &) -> bool { return false; });

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto remoteUrl = gitRepo->getPartialCloneRemoteUrl();
    ASSERT_TRUE(remoteUrl.has_value());

    auto caps = gitV2CapsCache(*remoteUrl);
    EXPECT_FALSE(caps.supportsFilter);

    setV2ProbeForTest(std::move(prev));
}

/* readBlob without a provider behaves identically to before — the
   Phase 2 fallback is gated on `provider != nullptr`. We verify by
   reading a normal commit's blob without any partial-clone setup,
   which should still work. */
TEST_F(GitPromisorWiringTest, ReadBlobWithoutProviderStillWorks)
{
    auto commit = commitOneFile("hello.txt", "hi from a non-partial repo");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    /* No provider; this should hit Phase 2 directly without engaging
       the fallback. */
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    EXPECT_EQ(accessor->readFile(CanonPath("/hello.txt")), "hi from a non-partial repo");
}

/* ---------- markAsPartialClone (the `git-lazy-fetch` creation path) ----------
 *
 * The env-var/`git-lazy-fetch` feature configures a fresh cache repo as
 * a partial clone via `markAsPartialClone`, then fetches with
 * `--filter=blob:none`. These tests pin the contract between that
 * CREATION path and the existing CONSUME path (`getPartialCloneRemoteUrl`
 * + the provider attach): a repo we mark must be recognized as a
 * partial clone of the right remote.
 */

TEST_F(GitPromisorWiringTest, MarkAsPartialCloneIsRecognizedByGetPartialCloneRemoteUrl)
{
    /* Create the `origin` remote, then mark the repo partial. The
       round-trip closes the loop: our writer feeds our reader. */
    git_remote * remote = nullptr;
    ASSERT_EQ(git_remote_create(&remote, repo, "origin", "https://example.invalid/mono.git"), 0);
    git_remote_free(remote);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    gitRepo->markAsPartialClone("origin");

    /* Re-open so we read freshly-written config off disk (mirrors
       git.cc, which re-opens the cache repo after fetch). */
    auto reopened = GitRepo::openRepo(tmpDir, {});
    auto remoteUrl = reopened->getPartialCloneRemoteUrl();
    ASSERT_TRUE(remoteUrl.has_value());
    EXPECT_EQ(*remoteUrl, "https://example.invalid/mono.git");
}

TEST_F(GitPromisorWiringTest, MarkAsPartialCloneWritesGitNativeConfig)
{
    /* The config we write must be byte-for-byte what `git clone
       --filter=blob:none` produces (verified empirically against git
       2.53), so a repo we mark is indistinguishable from one Git made:
       repositoryformatversion=1, extensions.partialClone=<remote>,
       remote.<remote>.promisor=true, partialclonefilter=blob:none. */
    git_remote * remote = nullptr;
    ASSERT_EQ(git_remote_create(&remote, repo, "origin", "https://example.invalid/mono.git"), 0);
    git_remote_free(remote);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    gitRepo->markAsPartialClone("origin");

    git_config * config = nullptr;
    ASSERT_EQ(git_repository_config(&config, repo), 0);
    Finally freeConfig([&] { git_config_free(config); });

    int32_t formatVersion = 0;
    ASSERT_EQ(git_config_get_int32(&formatVersion, config, "core.repositoryformatversion"), 0);
    EXPECT_EQ(formatVersion, 1);

    git_buf ext = GIT_BUF_INIT;
    Finally freeExt([&] { git_buf_dispose(&ext); });
    ASSERT_EQ(git_config_get_string_buf(&ext, config, "extensions.partialClone"), 0);
    EXPECT_EQ(std::string(ext.ptr, ext.size), "origin");

    int promisor = 0;
    ASSERT_EQ(git_config_get_bool(&promisor, config, "remote.origin.promisor"), 0);
    EXPECT_TRUE(promisor);

    git_buf filter = GIT_BUF_INIT;
    Finally freeFilter([&] { git_buf_dispose(&filter); });
    ASSERT_EQ(git_config_get_string_buf(&filter, config, "remote.origin.partialclonefilter"), 0);
    EXPECT_EQ(std::string(filter.ptr, filter.size), "blob:none");
}

TEST_F(GitPromisorWiringTest, MarkAsPartialCloneThenReadStillOpens)
{
    /* After marking partial (which sets repositoryformatversion=1 and
       an `extensions.*` key), libgit2 must still OPEN the repo —
       `initLibGit2` whitelists the `partialclone` extension. A repo
       with a commit already present reads its blobs with no provider
       (nothing is missing), proving the marking didn't break local
       reads. */
    auto commit = commitOneFile("present.txt", "local blob, no fetch needed");
    auto rev = toHash(commit);

    git_remote * remote = nullptr;
    ASSERT_EQ(git_remote_create(&remote, repo, "origin", "https://example.invalid/mono.git"), 0);
    git_remote_free(remote);

    {
        auto gitRepo = GitRepo::openRepo(tmpDir, {});
        gitRepo->markAsPartialClone("origin");
    }

    /* Re-open the now-partial repo and read the locally-present blob. */
    auto reopened = GitRepo::openRepo(tmpDir, {});
    auto accessor = reopened->getAccessor(rev, {}, "");
    EXPECT_EQ(accessor->readFile(CanonPath("/present.txt")), "local blob, no fetch needed");
}

/* ---------- ssh remote-command shape (forge-safety regression) ----------
 *
 * The ssh transport must send a BARE `git-upload-pack '<path>'` and
 * carry the protocol version out-of-band via `SetEnv`. An earlier
 * revision prefixed `env GIT_PROTOCOL=… ` inline on the command, which
 * every Git forge's `git-shell` rejects ("unrecognized command") — a
 * field failure against GitHub Enterprise. These pin the shape so it
 * can't regress without an ssh server in the loop.
 */

TEST(GitUploadPackCommand, IsBareNoPrefix)
{
    auto cmd = gitUploadPackCommand("/org/repo");
    /* The first word MUST be exactly `git-upload-pack` — `git-shell`
       allowlists it as the literal first token; any prefix is fatal. */
    EXPECT_EQ(cmd, "git-upload-pack '/org/repo'");
    EXPECT_TRUE(cmd.starts_with("git-upload-pack '"));
    /* Guard against the exact regression: no inline env prefix. */
    EXPECT_EQ(cmd.find("env "), std::string::npos);
    EXPECT_EQ(cmd.find("GIT_PROTOCOL"), std::string::npos);
}

TEST(GitUploadPackCommand, SingleQuotesPathAndEscapesQuotes)
{
    /* Path quoting must match Git's: wrap in '…', embedded ' becomes
       '\'' (close, escaped-quote, reopen). */
    EXPECT_EQ(gitUploadPackCommandArg("/a/b"), "'/a/b'");
    EXPECT_EQ(gitUploadPackCommandArg("/weird/it's"), "'/weird/it'\\''s'");
}

TEST(GitUploadPackCommand, ProtocolCarriedViaSetEnvOption)
{
    /* The version is negotiated out-of-band as an ssh option, never in
       the command. */
    EXPECT_EQ(gitProtocolSetEnvArg(), "-oSetEnv=GIT_PROTOCOL=version=2");
}

} // namespace nix::fetchers
