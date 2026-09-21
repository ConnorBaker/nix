#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/git.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/archive.hh"
#include "nix/util/util.hh"
#include "nix/util/hash.hh"
#include "nix/store/store-open.hh"
#include "nix/store/globals.hh"
#include "nix/store/local-store.hh"
#include "nix/store/tests/temp-local-store.hh"
#include "nix/util/tests/source-accessor-gen.hh"
#include "nix/util/tests/setting-scopes.hh"

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#ifndef _WIN32
#  include <sys/stat.h>
#endif

#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

namespace nix {

namespace {

/* A small tree, optionally named as a whole. */
ref<MemorySourceAccessor> namedTree(std::optional<std::string> name)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->addFile(CanonPath("keep/x"), "x");
    accessor->addFile(CanonPath("keep/drop/y"), "y");
    accessor->addFile(CanonPath("drop/z"), "z");
    accessor->addFile(CanonPath("top"), "t");
    accessor->fingerprint = std::move(name);
    return accessor;
}

PathFilter rejectDrop = [](const std::string & path) { return !hasSuffix(path, "/drop"); };
PathFilter rejectTop = [](const std::string & path) { return !hasSuffix(path, "/top"); };

/* Fixtures are populated before traversal and never mutated during it. */
struct ConcurrentMemorySource final : MemorySourceAccessor
{
    bool allowsConcurrentTraversal() override
    {
        return true;
    }
};

} // namespace

TEST(FixedSetFilter, namesByTheInnerNameAndTheAdmittedSet)
{
    auto inner = namedTree("T");
    auto set = filteredPaths(*inner, CanonPath::root, rejectDrop);

    auto w1 = makeFixedSetFilteringSourceAccessor(SourcePath(inner), set);
    auto w2 = makeFixedSetFilteringSourceAccessor(SourcePath(namedTree("T")), set);
    auto name1 = w1->getFingerprint(CanonPath::root);
    ASSERT_TRUE(name1.second);
    EXPECT_EQ(name1.first, CanonPath::root);
    /* Same inner name, same set: same name. */
    EXPECT_EQ(name1, w2->getFingerprint(CanonPath::root));

    /* Another set, another name. */
    auto w3 = makeFixedSetFilteringSourceAccessor(SourcePath(inner), filteredPaths(*inner, CanonPath::root, rejectTop));
    EXPECT_NE(name1.second, w3->getFingerprint(CanonPath::root).second);

    /* Another inner name, another name. */
    auto w4 = makeFixedSetFilteringSourceAccessor(SourcePath(namedTree("U")), set);
    EXPECT_NE(name1.second, w4->getFingerprint(CanonPath::root).second);

    /* A subtree is named by the set below it; at a subtree the filter
       does not touch, the name still differs from the inner name, since
       it says the set is the whole. */
    auto atKeep = w1->getFingerprint(CanonPath("keep"));
    ASSERT_TRUE(atKeep.second);
    EXPECT_EQ(atKeep.first, CanonPath::root);
    EXPECT_NE(atKeep.second, name1.second);

    /* An unnamed inner tree gives an unnamed filtered tree. */
    auto w5 = makeFixedSetFilteringSourceAccessor(SourcePath(namedTree(std::nullopt)), set);
    EXPECT_FALSE(w5->getFingerprint(CanonPath::root).second);

    /* A whole-tree name, when given, wins. */
    w5->fingerprint = "whole";
    EXPECT_EQ(w5->getFingerprint(CanonPath("keep")).second, std::optional<std::string>("whole"));
}

TEST(AllowListFilter, admissionAndNamingMatchEveryPrefixSubsetAndGrowth)
{
    using Components = std::vector<std::string>;
    const std::vector<Components> domain{{}, {"a"}, {"a", "b"}, {"a", "b", "c"}, {"a!"}, {"a\xc3\xa9"}, {"z"}};
    auto queries = domain;
    queries.insert(queries.end(), {{"a", "d"}, {"a!", "b"}, {"outside"}});
    auto isPrefix = [](const Components & a, const Components & b) {
        return a.size() <= b.size() && std::equal(a.begin(), a.end(), b.begin());
    };
    auto canon = [](const Components & components) {
        auto path = CanonPath::root;
        for (auto & component : components)
            path.push(component);
        return path;
    };

    struct Observed : MemorySourceAccessor
    {
        enum Outcome { Named, Unnamed, Missing, Throwing } outcome;
        size_t names = 0, stats = 0;
        std::optional<CanonPath> requested;

        std::optional<Stat> maybeLstat(const CanonPath & path) override
        {
            ++stats;
            return MemorySourceAccessor::maybeLstat(path);
        }

        std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
        {
            ++names;
            requested = path;
            if (outcome == Missing)
                lstat(path); // The empty memory accessor has no root.
            if (outcome == Throwing)
                throw Error("inner naming failed");
            if (outcome == Named)
                return {CanonPath("relative"), "inner"};
            return {path, std::nullopt};
        }
    };
    auto inner = make_ref<Observed>();

    /* The oracle compares component lists, independently of CanonPath's
       ordering and of the representation chosen for admitted prefixes. */
    for (size_t subset = 0; subset < (size_t{1} << domain.size()); ++subset) {
        std::set<CanonPath> initial;
        for (size_t i = 0; i < domain.size(); ++i)
            if (subset & (size_t{1} << i))
                initial.insert(canon(domain[i]));
        for (size_t added = 0; added <= domain.size(); ++added) {
            SCOPED_TRACE(fmt("subset %d, added %d", subset, added));
            auto accessor = AllowListSourceAccessor::create(inner, initial, {}, [](const CanonPath & path) {
                return RestrictedPathError("denied '%s'", path);
            });
            if (added < domain.size())
                accessor->allowPrefix(canon(domain[added]));
            for (auto & query : queries) {
                auto path = canon(query);
                SCOPED_TRACE(path.abs());
                bool covered = false, admitted = false;
                for (size_t i = 0; i < domain.size(); ++i)
                    if ((subset & (size_t{1} << i)) || i == added) {
                        covered |= isPrefix(domain[i], query);
                        admitted |= isPrefix(domain[i], query) || isPrefix(query, domain[i]);
                    }
                EXPECT_EQ(accessor->isAllowed(path), admitted);
                for (auto outcome : {Observed::Named, Observed::Unnamed, Observed::Missing, Observed::Throwing}) {
                    SCOPED_TRACE(outcome);
                    inner->outcome = outcome;
                    inner->names = inner->stats = 0;
                    inner->requested.reset();
                    if (covered && outcome == Observed::Missing)
                        EXPECT_THROW(accessor->getFingerprint(path), FileNotFound);
                    else if (covered && outcome == Observed::Throwing)
                        EXPECT_THROW(accessor->getFingerprint(path), Error);
                    else {
                        auto result = accessor->getFingerprint(path);
                        EXPECT_EQ(result.first, covered && outcome == Observed::Named ? CanonPath("relative") : path);
                        EXPECT_EQ(result.second, covered && outcome == Observed::Named ? std::optional<std::string>("inner") : std::nullopt);
                    }
                    EXPECT_EQ(inner->names, covered ? 1 : 0);
                    EXPECT_EQ(inner->stats, covered && outcome == Observed::Missing ? 1 : 0);
                    EXPECT_EQ(inner->requested, covered ? std::optional<CanonPath>(path) : std::nullopt);
                }
            }
        }
    }
}

TEST(AllowListFilter, exactPathsDoNotAdmitOrNameTheirRelatives)
{
    struct NoNaming : MemorySourceAccessor
    {
        std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath &) override
        {
            ADD_FAILURE() << "exact admission must not consult inner naming";
            throw Error("unexpected inner naming");
        }
    };
    size_t errors = 0;
    const auto exact = CanonPath("a/b");
    auto accessor = AllowListSourceAccessor::create(make_ref<NoNaming>(), {}, {exact}, [&](const CanonPath &) {
        ++errors;
        return RestrictedPathError("denied");
    });
    for (auto & path : {CanonPath::root, CanonPath("a"), exact, CanonPath("a/b/c"), CanonPath("a!"), CanonPath("other")}) {
        EXPECT_EQ(accessor->isAllowed(path), path == exact);
        if (path == exact)
            EXPECT_NO_THROW(accessor->checkAccess(path));
        else
            EXPECT_THROW(accessor->checkAccess(path), RestrictedPathError);
        auto result = accessor->getFingerprint(path);
        EXPECT_EQ(result.first, path);
        EXPECT_FALSE(result.second);
    }
    EXPECT_EQ(errors, 5);
    accessor->allowPrefix(CanonPath("a"));
    accessor->fingerprint = "whole";
    for (auto & path : {CanonPath::root, exact, CanonPath("other")}) {
        auto result = accessor->getFingerprint(path);
        EXPECT_EQ(result.first, path);
        EXPECT_EQ(result.second, std::optional<std::string>("whole"));
    }
}

/* The fetcher cache under a directory of the fixture's own (`FreshCacheHome`):
   these tests write memo rows. */
class FetchToStoreTest : FreshCacheHome, public ::testing::Test
{
protected:
    void SetUp() override
    {
        nix::initLibStore(/*loadConfig=*/false);
    }
};

TEST_F(FetchToStoreTest, filteredNamedTreesAreMemoised)
{
    auto store = openStore("dummy://?read-only=false");
    fetchers::Settings settings;

    auto inner = namedTree("fetch-to-store-test-T");
    auto method = ContentAddressMethod::Raw::NixArchive;

    auto [storePath, hash] =
        fetchToStore2(settings, *store, SourcePath(inner), FetchMode::DryRun, "src", method, &rejectDrop);

    /* The result is what the filtered dump gives. */
    auto [expectedPath, expectedHash] =
        store->computeStorePath("src", SourcePath(inner), method, HashAlgorithm::SHA256, {}, rejectDrop);
    EXPECT_EQ(storePath, expectedPath);
    EXPECT_EQ(hash, expectedHash);

    /* And the memo now has it under the filtered tree's name. */
    auto wrapped =
        makeFixedSetFilteringSourceAccessor(SourcePath(inner), filteredPaths(*inner, CanonPath::root, rejectDrop));
    auto [subpath, name] = wrapped->getFingerprint(CanonPath::root);
    ASSERT_TRUE(name);
    auto row = settings.getCache()->lookup(makeSourcePathToHashCacheKey(*name, method, subpath));
    ASSERT_TRUE(row);
    EXPECT_EQ(fetchers::getStrAttr(*row, "hash"), hash.to_string(HashFormat::SRI, true));

    /* The same filtered tree under another whole-tree name is another
       memo row; a second request for the first is answered from the memo
       (the store path and hash agree, and nothing new is walked here that
       this test can see). */
    auto [storePath2, hash2] =
        fetchToStore2(settings, *store, SourcePath(inner), FetchMode::DryRun, "src", method, &rejectDrop);
    EXPECT_EQ(storePath2, storePath);
    EXPECT_EQ(hash2, hash);
}

namespace {

std::string base(const std::string & path)
{
    auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

/* Construction soundness at any prefix: dumping the fixed-set accessor with
   no filter is dumping the inner tree with that filter, at the root and at
   every directory below.  This is the example that caught the coordinate
   bug (05 section 11) generalised over random trees, filters and prefixes. */
RC_GTEST_PROP(FetchToStore, prop_wrapped_dump_equals_filtered_dump, ())
{
    auto inner = make_ref<MemorySourceAccessor>();
    auto names = std::vector<std::string>{"a", "b", "c", "d"};
    auto n = *rc::gen::inRange<int>(1, 6);
    for (int i = 0; i < n; ++i)
        inner->addFile(
            CanonPath(*rc::gen::elementOf(names)) / *rc::gen::elementOf(names), *rc::gen::arbitrary<std::string>());
    inner->addFile(CanonPath("top"), *rc::gen::arbitrary<std::string>());

    /* A random subset of `names`.  `gen::container<std::set>` over this
       four-element domain gives up once rapidcheck's size parameter asks
       for more distinct values than exist (it cannot draw 41 from 4),
       which failed the property on Linux under a seed that grew the size;
       drawing a subset directly cannot exhaust. */
    std::set<std::string> rejected;
    for (auto & nm : names)
        if (*rc::gen::arbitrary<bool>())
            rejected.insert(nm);
    PathFilter filter = [&](const std::string & path) { return !rejected.contains(base(path)); };

    std::vector<CanonPath> dirs{CanonPath::root};
    std::function<void(const CanonPath &)> walk = [&](const CanonPath & p) {
        for (auto & [name, type] : inner->readDirectory(p)) {
            auto c = p / name;
            if (inner->lstat(c).type == SourceAccessor::tDirectory) {
                dirs.push_back(c);
                walk(c);
            }
        }
    };
    walk(CanonPath::root);

    for (auto & q : dirs) {
        auto wrapped = makeFixedSetFilteringSourceAccessor(SourcePath(inner, q), filteredPaths(*inner, q, filter));
        RC_ASSERT(narOf(*wrapped, CanonPath::root) == narOf(*inner, q, filter));
    }
}

/* Naming homomorphism: two inner trees that share a subtree, filtered the
   same way, name the shared filtered subtree equally and it has the same
   NAR; a different accepted set gives a different name. */
RC_GTEST_PROP(FetchToStore, prop_filtered_names_are_sound, ())
{
    auto keepContent = *rc::gen::arbitrary<std::string>();
    auto dropContent = *rc::gen::arbitrary<std::string>();
    auto build = [&](std::string priv) {
        auto mem = make_ref<MemorySourceAccessor>();
        mem->addFile(CanonPath("shared/keep"), std::string(keepContent));
        mem->addFile(CanonPath("shared/drop"), std::string(dropContent));
        mem->addFile(CanonPath(priv), std::string(priv));
        return make_ref<ContentNamedAccessor>(mem).cast<SourceAccessor>();
    };
    auto t1 = build("x"), t2 = build("y");

    PathFilter keepOnly = [](const std::string & p) { return base(p) != "drop"; };
    auto w1 = makeFixedSetFilteringSourceAccessor(
        SourcePath(t1, CanonPath("shared")), filteredPaths(*t1, CanonPath("shared"), keepOnly));
    auto w2 = makeFixedSetFilteringSourceAccessor(
        SourcePath(t2, CanonPath("shared")), filteredPaths(*t2, CanonPath("shared"), keepOnly));

    auto name1 = w1->getFingerprint(CanonPath::root);
    RC_ASSERT(name1.second);
    RC_ASSERT(name1 == w2->getFingerprint(CanonPath::root));
    RC_ASSERT(narOf(*w1, CanonPath::root) == narOf(*w2, CanonPath::root));

    auto full = makeFixedSetFilteringSourceAccessor(
        SourcePath(t1, CanonPath("shared")), filteredPaths(*t1, CanonPath("shared"), defaultPathFilter));
    RC_ASSERT(full->getFingerprint(CanonPath::root).second != name1.second);
}

/* Whole-view fingerprints cache each requested subtree. Descendants are
   not independently reusable across revisions of that view, so merely
   visiting one no longer eagerly constructs its persistent cache row. */
TEST_F(FetchToStoreTest, everyRequestedSubtreeIsReadableWhenTheWalkReturns)
{
    auto store = openStore("dummy://?read-only=false");
    fetchers::Settings settings;
    auto git = ContentAddressMethod::Raw::Git;
    auto tree = namedTree(
        "fetch-to-store-test-batch-" + Hash::random(HashAlgorithm::SHA256).to_string(HashFormat::Nix32, false));
    fetchToStore2(settings, *store, SourcePath(tree), FetchMode::DryRun, "src", git);

    auto [subpath, name] = tree->getFingerprint(CanonPath("keep"));
    ASSERT_TRUE(name);
    auto row = settings.getCache()->lookup(makeSourcePathToHashCacheKey(*name, git, subpath));
    EXPECT_FALSE(row);
    fetchToStore2(settings, *store, SourcePath(tree, CanonPath("keep")), FetchMode::DryRun, "src", git);
    row = settings.getCache()->lookup(makeSourcePathToHashCacheKey(*name, git, subpath));
    ASSERT_TRUE(row);
    EXPECT_EQ(Hash::parseSRI(fetchers::getStrAttr(*row, "hash")), objectHashOf(*tree, CanonPath("keep")).root.hash);

    /* And that row answers the next naming: a fresh in-process state over
       the same database reads it rather than the tree. */
    fetchers::Settings settings2;
    auto [p2, h2] = fetchToStore2(settings2, *store, SourcePath(tree), FetchMode::DryRun, "src", git);
    EXPECT_EQ(h2, objectHashOf(*tree, CanonPath::root).root.hash);
}

namespace {

/* A memory tree under a fixed name, with or without one extra entry.  The
   name is the memo's key, so a tree that changes under it shows exactly
   when the memo is consulted rather than the tree. */
ref<MemorySourceAccessor> treeNamed(const std::string & name, bool extra)
{
    auto a = make_ref<MemorySourceAccessor>();
    a->addFile(CanonPath("dir/keep"), "k");
    a->addFile(CanonPath("dir/drop"), "d");
    a->addFile(CanonPath("file"), "f");
    if (extra)
        a->addFile(CanonPath("dir/extra"), "e");
    a->fingerprint = name;
    return a;
}

/* An unnamed root with the named tree mounted below it: `rootFS` above a
   lazily mounted input. */
SourcePath mountedUnder(ref<SourceAccessor> tree)
{
    auto root = make_ref<MemorySourceAccessor>();
    root->addFile(CanonPath("plain"), "p");
    return SourcePath(makeMountedSourceAccessor({{CanonPath::root, root}, {CanonPath("m"), tree}}));
}

} // namespace

/* The subtree memo of the compositional name (01 section 9.9): an
   unfiltered walk answers a named subtree from the memo; a walk under a
   filter never answers a directory from it, since the filter decides the
   directory's entries, while a file's blob is the same under any filter. */
TEST_F(FetchToStoreTest, gitSubtreeMemoRespectsFilters)
{
    auto store = openStore("dummy://?read-only=false");
    fetchers::Settings settings;
    auto method = ContentAddressMethod::Raw::Git;
    /* This test poisons the memo on purpose: a name over a changed tree. */
    auto name = "fetch-to-store-test-memo-" + Hash::random(HashAlgorithm::SHA256).to_string(HashFormat::Nix32, false);
    PathFilter dropDrop = [](const std::string & p) { return !hasSuffix(p, "/drop"); };

    /* An unfiltered walk names the tree and every subtree under `name`. */
    auto t1 = mountedUnder(treeNamed(name, false));
    auto [p1, h1] = fetchToStore2(settings, *store, t1, FetchMode::DryRun, "src", method);
    EXPECT_EQ(h1, objectHashOf(*t1.accessor, t1.path).root.hash);

    /* The same name over a tree with one more entry: unfiltered, the
       subtree is answered from the memo, so the old hash comes back. */
    auto t2 = mountedUnder(treeNamed(name, true));
    auto [p2, h2] = fetchToStore2(settings, *store, t2, FetchMode::DryRun, "src", method);
    EXPECT_EQ(h2, h1);
    EXPECT_NE(h1, objectHashOf(*t2.accessor, t2.path).root.hash);

    /* Under a filter the directories are walked: the hash is the filtered
       tree's own, with `extra` in and `drop` out. */
    auto [p3, h3] = fetchToStore2(settings, *store, t2, FetchMode::DryRun, "src", method, &dropDrop);
    EXPECT_EQ(h3, objectHashOf(*t2.accessor, t2.path, dropDrop).root.hash);
    EXPECT_NE(h3, objectHashOf(*t1.accessor, t1.path, dropDrop).root.hash);
}

/* A bare root is addressed by the object hash (doc/lazy-store/
   01-specification.md, section 9.11): the blob id for a plain file, and for
   an executable file or a symlink the id of the one-entry tree `{"." ->
   entry}`, which keeps the mode.  Total, so the git method names every
   root kind, and injective, so a plain file, an executable and a symlink
   with the same bytes are three paths. */
/* A git row of the earlier form -- the hash alone, under the key without
   `version` -- is a miss (`01` §9.9, the memo's rule): a wrong hash planted there
   is not read, and the true address is computed.  A row of the current
   form is read; a bare root's row holds its entry, so no stat of the root
   is needed to address it. */
TEST_F(FetchToStoreTest, oldFormGitRowIsAMissAndTheRowIsTheEntry)
{
    auto store = openStore("dummy://?read-only=false");
    auto git = ContentAddressMethod::Raw::Git;
    auto tree = namedTree("fetch-to-store-test-V");
    auto expected = objectHashOf(*tree, CanonPath::root).root;
    auto [subpath, name] = tree->getFingerprint(CanonPath::root);
    ASSERT_TRUE(name);

    fetchers::Settings settings;
    auto wrong = merkle::treeId("");
    settings.getCache()->upsert(
        fetchers::Cache::Key{
            "sourcePathToHash",
            {{"fingerprint", *name},
             {"method", std::string{ContentAddressMethod(git).render()}},
             {"path", subpath.abs()}}},
        {{"hash", wrong.to_string(HashFormat::SRI, true)}});
    auto [p, h] = fetchToStore2(settings, *store, SourcePath(tree), FetchMode::DryRun, "src");
    EXPECT_EQ(h, expected.hash);
    EXPECT_NE(h, wrong);

    /* The row written is the entry, and it answers the next naming. */
    auto row = settings.getCache()->lookup(makeSourcePathToHashCacheKey(*name, git, subpath));
    ASSERT_TRUE(row);
    EXPECT_EQ(Hash::parseSRI(fetchers::getStrAttr(*row, "hash")), expected.hash);
    EXPECT_EQ(fetchers::getIntAttr(*row, "mode"), uint64_t(merkle::Mode::Directory));
    fetchers::Settings settings2;
    settings2.getCache()->upsert(makeSourcePathToHashCacheKey(*name, git, subpath), *row);
    auto [p2, h2] = fetchToStore2(settings2, *store, SourcePath(tree), FetchMode::DryRun, "src");
    EXPECT_EQ(p2, p);
    EXPECT_EQ(h2, h);
}

namespace {

/* A memory tree whose nodes carry the names given (`names`, a fingerprint
   per path, as a git accessor names its subtrees by object id), counting
   the files read of it: the copy route's promise is that nothing beneath
   a named subtree the store holds is read (01 §9.10, the second route). */
struct NamedCountingAccessor : MemorySourceAccessor
{
    std::map<CanonPath, std::string> names;
    /* Atomic: the parallel copy reads on its workers as well as on the
       walking thread (`08` K17), so a plain counter races.  Found by the
       thread-sanitised run of `05` section 5. */
    std::atomic<unsigned> reads = 0;

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        reads++;
        MemorySourceAccessor::readFile(path, sink, sizeCallback);
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        if (auto i = names.find(path); i != names.end())
            return {CanonPath::root, i->second};
        return {path, std::nullopt};
    }
};

ref<NamedCountingAccessor> dirtyTree(std::string_view b, std::string_view dName)
{
    auto t = make_ref<NamedCountingAccessor>();
    t->addFile(CanonPath("a"), "one");
    t->addFile(CanonPath("d/b"), std::string(b));
    t->addFile(CanonPath("d/c"), "three");
    t->addFile(CanonPath("e/x"), "four");
    t->addFile(CanonPath("e/y"), "five");
    /* The shape of a dirty checkout: the root and the changed directory
       carry no name the memo could answer, the clean directory does. */
    t->names.emplace(CanonPath("d"), std::string(dName));
    t->names.emplace(CanonPath("e"), "fetch-to-store-test-e-clean");
    return t;
}

} // namespace

TEST_F(FetchToStoreTest, subtreeNamingIsObservedOnceAndRecordedForTheSameNode)
{
    struct ObservedNames : NamedCountingAccessor
    {
        std::map<CanonPath, size_t> observations;

        std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
        {
            ++observations[path];
            return NamedCountingAccessor::getFingerprint(path);
        }
    };

    auto store = openStore("dummy://?read-only=false");
    for (bool named : {false, true})
        for (bool filtered : {false, true}) {
            SCOPED_TRACE(fmt("named=%d filtered=%d", named, filtered));
            fetchers::Settings settings;
            auto tree = make_ref<ObservedNames>();
            tree->addFile(CanonPath("plain"), "plain");
            tree->addFile(CanonPath("nested/child"), "child");
            tree->addFile(CanonPath("nested/drop"), "drop");
            MemorySink sink{*tree};
            sink.createDirectory(CanonPath("empty"));
            sink.createSymlink(CanonPath("link"), "target");
            sink.createRegularFile(CanonPath("exec"), [](CreateRegularFileSink & file) {
                file.isExecutable();
                file("executable");
            });
            std::vector<CanonPath> nodes{
                CanonPath("plain"),
                CanonPath("exec"),
                CanonPath("link"),
                CanonPath("empty"),
                CanonPath("nested"),
                CanonPath("nested/child"),
                CanonPath("nested/drop")};
            if (named)
                for (auto & path : nodes)
                    tree->names.emplace(path, fmt("transient-%d-%s", filtered, path.abs()));

            auto & filter = filtered ? rejectDrop : defaultPathFilter;
            auto expected = merkle::objectHash(objectHashOf(*tree, CanonPath::root, filter).root);
            auto actual = fetchToStore2(
                settings, *store, SourcePath(tree), FetchMode::DryRun, "source", {}, filtered ? &filter : nullptr);
            EXPECT_EQ(actual.second, expected);
            EXPECT_EQ(tree->observations[CanonPath::root], 1u);
            for (auto & path : nodes) {
                SCOPED_TRACE(path.abs());
                bool omitted = filtered && path == CanonPath("nested/drop");
                bool directory = tree->lstat(path).type == SourceAccessor::tDirectory;
                bool recorded = !omitted && !(filtered && directory);
                EXPECT_EQ(tree->observations[path], recorded ? 1u : 0u);
                if (named) {
                    auto row = settings.getCache()->lookup(makeSourcePathToHashCacheKey(
                        tree->names.at(path), ContentAddressMethod::Raw::Git, CanonPath::root));
                    ASSERT_EQ(row.has_value(), recorded);
                    if (row) {
                        auto entry = objectHashOf(*tree, path).root;
                        EXPECT_EQ(Hash::parseSRI(fetchers::getStrAttr(*row, "hash")), entry.hash);
                        EXPECT_EQ(fetchers::getIntAttr(*row, "mode"), static_cast<uint64_t>(entry.mode));
                    }
                }
            }
            EXPECT_EQ(
                fetchToStore2(
                    settings, *store, SourcePath(tree), FetchMode::DryRun, "source", {}, filtered ? &filter : nullptr)
                    .second,
                expected);
            EXPECT_EQ(tree->observations[CanonPath::root], 2u);
        }
}

TEST_F(FetchToStoreTest, repairDryRunReadsEveryRootKindDespiteNamesAndKnownEntries)
{
    auto store = openStore("dummy://?read-only=false");
    size_t serial = 0;
    PathFilter admitAll = [](const std::string &) { return true; };
    for (auto kind : {merkle::Mode::Regular, merkle::Mode::Executable, merkle::Mode::Symlink, merkle::Mode::Directory})
        for (bool known : {false, true})
            for (bool nested : {false, true})
                for (bool filtered : {false, true}) {
                    SCOPED_TRACE(fmt(
                        "kind=%d known=%d nested=%d filtered=%d", static_cast<RawMode>(kind), known, nested, filtered));
                    auto makeTree = [&](std::string_view text) {
                        auto tree = make_ref<MemorySourceAccessor>();
                        MemorySink sink{*tree};
                        if (kind == merkle::Mode::Symlink)
                            sink.createSymlink(CanonPath::root, std::string(text));
                        else {
                            if (kind == merkle::Mode::Directory)
                                sink.createDirectory(CanonPath::root);
                            sink.createRegularFile(
                                kind == merkle::Mode::Directory ? CanonPath("child") : CanonPath::root,
                                [&](CreateRegularFileSink & file) {
                                    if (kind == merkle::Mode::Executable)
                                        file.isExecutable();
                                    file(text);
                                });
                        }
                        return tree;
                    };
                    auto before = makeTree("before");
                    auto after = makeTree("after");
                    /* Deliberately retain an obsolete claim, as when repair
                       encounters corrupt bytes behind an immutable name. */
                    if (known)
                        before->knownRoot = after->knownRoot = objectHashOf(*before, CanonPath::root).root;
                    else
                        before->fingerprint = after->fingerprint = fmt("repair-dry-run-%d", serial++);
                    auto first = nested ? mountedUnder(before) : SourcePath(before);
                    auto second = nested ? mountedUnder(after) : SourcePath(after);
                    fetchers::Settings settings;
                    auto seed = fetchToStore2(
                        settings, *store, first, FetchMode::DryRun, "source", {}, filtered ? &admitAll : nullptr);
                    auto expected = merkle::objectHash(objectHashOf(*second.accessor, second.path).root);
                    EXPECT_NE(seed.second, expected);
                    auto repaired = fetchToStore2(
                        settings,
                        *store,
                        second,
                        FetchMode::DryRun,
                        "source",
                        {},
                        filtered ? &admitAll : nullptr,
                        Repair);
                    EXPECT_EQ(repaired.second, expected);
                }
}

/* The fetcher cache and a local store of the fixture's own. */
class FetchToStoreLocalTest : FreshCacheHome, public TempLocalStoreTest
{
protected:
    void SetUp() override
    {
        nix::initLibStore(/*loadConfig=*/false);
        TempLocalStoreTest::SetUp();
    }
};

TEST_F(FetchToStoreLocalTest, positiveRootNamingIsObservedOnceForEveryRootKindAndMode)
{
    struct ObservedNames : MemorySourceAccessor
    {
        std::string name;
        std::map<CanonPath, size_t> observations;

        std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
        {
            ++observations[path];
            return {path, name};
        }
    };

    size_t serial = 0;
    const CanonPath root("view");
    for (auto kind : {merkle::Mode::Regular, merkle::Mode::Executable, merkle::Mode::Symlink, merkle::Mode::Directory})
        for (auto mode : {FetchMode::DryRun, FetchMode::Copy})
            /* The whole-view assertion is absent, equal to the returned
               name, or another name for the same tree. Only equality
               chooses the root-only memo scope. */
            for (size_t state = 0; state < 3; ++state) {
                SCOPED_TRACE(fmt("kind=%d copy=%d state=%d", static_cast<RawMode>(kind), mode == FetchMode::Copy, state));
                fetchers::Settings settings;
                auto tree = make_ref<ObservedNames>();
                tree->name = fmt("observed-positive-root-%d", serial++);
                if (state != 0)
                    tree->fingerprint = state == 1 ? tree->name : "another-" + tree->name;
                MemorySink sink{*tree};
                sink.createDirectory(CanonPath::root);
                if (kind == merkle::Mode::Directory)
                    tree->addFile(root / CanonPath("nested/child"), "contents");
                else if (kind == merkle::Mode::Symlink)
                    sink.createSymlink(root, "target");
                else
                    sink.createRegularFile(root, [&](CreateRegularFileSink & file) {
                        if (kind == merkle::Mode::Executable)
                            file.isExecutable();
                        file("contents");
                    });
                SourcePath source{tree, root};
                auto entry = objectHashOf(*tree, root).root;
                auto result = fetchToStore2(settings, *store, source, mode);
                EXPECT_EQ(result.second, merkle::objectHash(entry));
                EXPECT_EQ(tree->observations[root], 1u);
                if (kind == merkle::Mode::Directory) {
                    EXPECT_EQ(
                        tree->observations[root / "nested"],
                        state != 1 && (mode == FetchMode::DryRun || store->materialisesFromObjects()) ? 1u : 0u);
                    EXPECT_EQ(
                        tree->observations[root / CanonPath("nested/child")],
                        state != 1 && mode == FetchMode::DryRun ? 1u : 0u);
                }
                /* A dry run records the exact entry. Copy has only the
                   object hash, which recovers plain-file/directory entries. */
                auto row = settings.getCache()->lookup(
                    makeSourcePathToHashCacheKey(tree->name, ContentAddressMethod::Raw::Git, root));
                bool recorded = mode == FetchMode::DryRun || kind == merkle::Mode::Regular || kind == merkle::Mode::Directory;
                ASSERT_EQ(row.has_value(), recorded);
                if (row) {
                    EXPECT_EQ(Hash::parseSRI(fetchers::getStrAttr(*row, "hash")), entry.hash);
                    EXPECT_EQ(fetchers::getIntAttr(*row, "mode"), static_cast<uint64_t>(entry.mode));
                }
                EXPECT_EQ(fetchToStore2(settings, *store, source, mode), result);
                EXPECT_EQ(tree->observations[root], 2u);
            }
}

TEST_F(FetchToStoreLocalTest, namingCacheDoesNotOwnItsSource)
{
    fetchers::Settings fetchSettings;
    std::weak_ptr<SourceAccessor> weak;
    {
        auto source = namedTree("cache-source-lifetime");
        weak = source.get_ptr();
        fetchToStore2(fetchSettings, *store, SourcePath(source), FetchMode::DryRun, "source");
    }
    EXPECT_TRUE(weak.expired());
}

TEST_F(FetchToStoreLocalTest, cachedNamesAreInterpretedInTheDestinationStore)
{
    fetchers::Settings fetchSettings;
    size_t serial = 0;
    for (bool sameDirectory : {false, true}) {
        auto destinationConfig = std::make_shared<LocalStoreConfig>(
            tempStoreDir.path() / (sameDirectory ? "same-dir" : "other-dir"),
            StoreConfig::Params{{"store", store->storeDir + (sameDirectory ? "" : "-other")}});
        auto destination = std::make_shared<LocalStore>(ref{destinationConfig});
        for (auto method :
             {ContentAddressMethod::Raw::Flat,
              ContentAddressMethod::Raw::NixArchive,
              ContentAddressMethod::Raw::Git,
              ContentAddressMethod::Raw::Text})
            for (auto first : {FetchMode::DryRun, FetchMode::Copy})
                for (auto second : {FetchMode::DryRun, FetchMode::Copy})
                    for (bool sameName : {false, true}) {
                        SCOPED_TRACE(
                            fmt("directory=%d method=%d first=%d second=%d name=%d",
                                sameDirectory,
                                static_cast<int>(method),
                                static_cast<int>(first),
                                static_cast<int>(second),
                                sameName));
                        auto source = make_ref<MemorySourceAccessor>();
                        source->root = MemorySourceAccessor::File::Regular{.contents = fmt("case-%d", serial++)};
                        source->fingerprint = fmt("destination-case-%d", serial);
                        SourcePath path{source};
                        auto before = fetchToStore2(fetchSettings, *store, path, first, "first", method);
                        auto name = sameName ? "first" : "second";
                        auto expected = destination->computeStorePath(name, path, method, HashAlgorithm::SHA256);
                        auto after = fetchToStore2(fetchSettings, *destination, path, second, name, method);
                        EXPECT_EQ(after, expected);
                        EXPECT_EQ(after.second, before.second);
                        EXPECT_EQ(destination->isValidPath(after.first), second == FetchMode::Copy);
                    }
    }
}

TEST_F(FetchToStoreLocalTest, namingFollowsTheCurrentGraftsAndAllowedPrefixes)
{
    fetchers::Settings settings;
    size_t serial = 0;
    auto check = [&](auto makeView) {
        /* Directories admit exactly these two methods. Each growth case
           crosses both naming/copy modes and both output-name relations. */
        for (auto method : {ContentAddressMethod::Raw::Git, ContentAddressMethod::Raw::NixArchive})
            for (auto first : {FetchMode::DryRun, FetchMode::Copy})
                for (auto second : {FetchMode::DryRun, FetchMode::Copy})
                    for (bool sameName : {false, true}) {
                        SCOPED_TRACE(
                            fmt("method=%d first=%d second=%d same-name=%d",
                                static_cast<int>(method),
                                static_cast<int>(first),
                                static_cast<int>(second),
                                sameName));
                        auto [accessor, grow] = makeView(++serial);
                        SourcePath source{accessor};
                        auto before = fetchToStore2(settings, *store, source, first, "before", method);
                        EXPECT_EQ(before, store->computeStorePath("before", source, method, HashAlgorithm::SHA256));
                        grow();
                        auto name = sameName ? "before" : "after";
                        auto expected = store->computeStorePath(name, source, method, HashAlgorithm::SHA256);
                        EXPECT_NE(before.second, expected.second);
                        auto after = fetchToStore2(settings, *store, source, second, name, method);
                        EXPECT_EQ(after, expected);
                        if (second == FetchMode::Copy) {
                            EXPECT_TRUE(store->isValidPath(after.first));
                            EXPECT_EQ(narOf(*store->requireStoreObjectAccessor(after.first)), narOf(*accessor));
                        }
                    }
    };

    for (bool namedBase : {false, true})
        for (bool namedGraft : {false, true}) {
            SCOPED_TRACE(fmt("mount: named-base=%d named-graft=%d", namedBase, namedGraft));
            check([&](size_t i) -> std::pair<ref<SourceAccessor>, std::function<void()>> {
                auto base = make_ref<MemorySourceAccessor>();
                base->addFile(CanonPath("before"), fmt("before-%d", i));
                if (namedBase)
                    base->fingerprint = fmt("growth-base-%d", i);
                auto graft = make_ref<MemorySourceAccessor>();
                graft->addFile(CanonPath("after"), fmt("after-%d", i));
                if (namedGraft)
                    graft->fingerprint = fmt("growth-graft-%d", i);
                auto mounted = makeMountedSourceAccessor({{CanonPath::root, base}});
                return {mounted, [mounted, graft] { mounted->mount(CanonPath("extra"), graft); }};
            });
        }

    for (bool namedInner : {false, true})
        for (bool admitWhole : {false, true}) {
            SCOPED_TRACE(fmt("allow-list: named-inner=%d admit-whole=%d", namedInner, admitWhole));
            check([&](size_t i) -> std::pair<ref<SourceAccessor>, std::function<void()>> {
                auto inner = make_ref<MemorySourceAccessor>();
                inner->addFile(CanonPath("before"), fmt("before-%d", i));
                inner->addFile(CanonPath("after"), fmt("after-%d", i));
                if (namedInner)
                    inner->fingerprint = fmt("growth-inner-%d", i);
                auto allowed =
                    AllowListSourceAccessor::create(inner, {CanonPath("before")}, {}, [](const CanonPath & path) {
                        return RestrictedPathError("access to '%s' is forbidden", path);
                    });
                return {allowed, [allowed, admitWhole] {
                            allowed->allowPrefix(admitWhole ? CanonPath::root : CanonPath("after"));
                        }};
            });
        }
}

TEST_F(FetchToStoreLocalTest, anUnnamedAccessorDoesNotPromiseStableContents)
{
    fetchers::Settings settings;
    size_t serial = 0;
    for (auto method :
         {ContentAddressMethod::Raw::Flat,
          ContentAddressMethod::Raw::NixArchive,
          ContentAddressMethod::Raw::Git,
          ContentAddressMethod::Raw::Text})
        for (auto first : {FetchMode::DryRun, FetchMode::Copy})
            for (auto second : {FetchMode::DryRun, FetchMode::Copy})
                for (bool sameName : {false, true}) {
                    SCOPED_TRACE(
                        fmt("method=%d first=%d second=%d same-name=%d",
                            static_cast<int>(method),
                            static_cast<int>(first),
                            static_cast<int>(second),
                            sameName));
                    auto source = make_ref<MemorySourceAccessor>();
                    source->root = MemorySourceAccessor::File::Regular{.contents = fmt("before-%d", ++serial)};
                    SourcePath path{source};
                    auto before = fetchToStore2(settings, *store, path, first, "before", method);
                    source->root = MemorySourceAccessor::File::Regular{.contents = fmt("after-%d", serial)};
                    auto name = sameName ? "before" : "after";
                    auto expected = store->computeStorePath(name, path, method, HashAlgorithm::SHA256);
                    EXPECT_NE(before.second, expected.second);
                    auto after = fetchToStore2(settings, *store, path, second, name, method);
                    EXPECT_EQ(after, expected);
                    if (second == FetchMode::Copy) {
                        EXPECT_EQ(
                            store->requireStoreObjectAccessor(after.first)->readFile(CanonPath::root), path.readFile());
                    }
                }
}

#ifndef _WIN32
TEST_F(FetchToStoreLocalTest, pathInputsPreserveTheTreeForEveryStoreStateAndWidth)
{
    auto savedFeatures = experimentalFeatureSettings.experimentalFeatures.get();
    Finally restoreFeatures{[&] { experimentalFeatureSettings.experimentalFeatures = savedFeatures; }};
    auto features = savedFeatures;
    features.insert(Xp::Flakes);
    experimentalFeatureSettings.experimentalFeatures = features;
    auto inputDir = tempStoreDir.path() / "path-source";
    createDirs(inputDir / "empty");
    for (size_t i = 0; i < 16; ++i) {
        auto dir = inputDir / fmt("nested/d%d", i);
        createDirs(dir);
        writeFile(dir / "plain", fmt("file %d", i));
        writeFile(dir / "exec", "executable");
        chmod(dir / "exec", 0755);
        createSymlink("plain", dir / "link");
    }
    auto input = fetchers::Input::fromAttrs({{"type", "path"}, {"path", inputDir.string()}});
    fetchers::Settings fetchSettings;
    Finally restoreWorkers{
        [old = LocalStore::copyWorkersForTest.load()] { LocalStore::copyWorkersForTest.store(old); }};
    for (size_t size : {size_t(4096), size_t(65 * 1024 * 1024)}) {
        writeFile(inputDir / "large", std::string(size, 'q'));
        for (size_t workers = 1; workers <= 8; ++workers) {
            SCOPED_TRACE(fmt("size=%d workers=%d", size, workers));
            LocalStore::copyWorkersForTest.store(workers);
            auto destinationConfig = std::make_shared<LocalStoreConfig>(
                tempStoreDir.path() / fmt("destination-%d-%d", size, workers), StoreConfig::Params{});
            auto destination = std::make_shared<LocalStore>(ref{destinationConfig});
            ASSERT_FALSE(destination->hasValidPaths());
            for (size_t state = 0; state < 3; ++state) {
                SCOPED_TRACE(fmt("state=%d (empty miss, populated hit, populated miss)", state));
                if (state == 2)
                    writeFile(inputDir / "nested/d0/plain", fmt("changed-%d-%d", size, workers));
                if (size == 4096) {
                    auto bad = inputDir / "nested/d7/unsupported";
                    ASSERT_EQ(mkfifo(bad.c_str(), 0600), 0);
                    EXPECT_THROW(input.getAccessor(fetchSettings, *destination), Error);
                    EXPECT_EQ(destination->hasValidPaths(), state != 0);
                    std::filesystem::remove(bad);
                }
                HashSink expected{HashAlgorithm::SHA256};
                makeFSSourceAccessor(inputDir)->dumpPath(CanonPath::root, expected);
                auto [accessor, fetched] = input.getAccessor(fetchSettings, *destination);
                HashSink actual{HashAlgorithm::SHA256};
                accessor->dumpPath(CanonPath::root, actual);
                EXPECT_EQ(actual.finish().hash, expected.finish().hash);
                EXPECT_TRUE(destination->hasValidPaths());
            }
        }
    }
}
#endif

TEST_F(FetchToStoreLocalTest, knownEntriesPreserveEveryRootKindAndRepairReadsTheSource)
{
    for (auto kind :
         {merkle::Mode::Regular, merkle::Mode::Executable, merkle::Mode::Symlink, merkle::Mode::Directory}) {
        SCOPED_TRACE(static_cast<RawMode>(kind));
        auto tree = make_ref<NamedCountingAccessor>();
        MemorySink sink{*tree};
        if (kind == merkle::Mode::Symlink)
            sink.createSymlink(CanonPath::root, "target");
        else {
            if (kind == merkle::Mode::Directory)
                sink.createDirectory(CanonPath::root);
            sink.createRegularFile(
                kind == merkle::Mode::Directory ? CanonPath("child") : CanonPath::root,
                [&](CreateRegularFileSink & file) {
                    if (kind == merkle::Mode::Executable)
                        file.isExecutable();
                    file("contents");
                });
        }
        auto entry = objectHashOf(*tree, CanonPath::root).root;
        tree->knownRoot = entry;
        tree->reads = 0;
        fetchers::Settings settings;
        auto named = fetchToStore2(settings, *store, SourcePath(tree), FetchMode::DryRun, "known");
        EXPECT_EQ(named.second, merkle::objectHash(entry));
        EXPECT_EQ(tree->reads, 0u);
        auto copied = fetchToStore2(settings, *store, SourcePath(tree), FetchMode::Copy, "known");
        EXPECT_EQ(copied, named);
        auto stored = store->requireStoreObjectAccessor(copied.first);
        EXPECT_TRUE(stored->getFingerprint(CanonPath::root).second);
        EXPECT_EQ(objectHashOf(*stored, CanonPath::root).root, entry);
        tree->reads = 0;
        fetchers::Settings fresh;
        EXPECT_EQ(fetchToStore2(fresh, *store, SourcePath(tree), FetchMode::Copy, "known"), named);
        EXPECT_EQ(tree->reads, 0u);

        /* A repair must bypass both the process cache and the known entry.
           Changing a regular holder in place also tests repair's refusal
           to relink its corrupt bytes over the fresh source. */
        if (kind != merkle::Mode::Symlink) {
            auto physical = store->toRealPath(copied.first);
            if (kind == merkle::Mode::Directory)
                physical /= "child";
            std::filesystem::permissions(
                physical, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
            writeFile(physical, "corrupt!");
        }
        EXPECT_EQ(fetchToStore2(fresh, *store, SourcePath(tree), FetchMode::Copy, "known", {}, nullptr, Repair), named);
        if (kind != merkle::Mode::Symlink)
            EXPECT_GT(tree->reads, 0u);
        EXPECT_EQ(objectHashOf(*store->requireStoreObjectAccessor(copied.first), CanonPath::root).root, entry);
    }
}

TEST_F(FetchToStoreTest, filteringAndGraftsInvalidateKnownDirectoryEntries)
{
    auto store = openStore("dummy://?read-only=false");
    auto tree = namedTree("known-filter-root");
    tree->knownRoot = objectHashOf(*tree, CanonPath::root).root;
    fetchers::Settings settings;
    auto filtered = fetchToStore2(settings, *store, SourcePath(tree), FetchMode::DryRun, "filtered", {}, &rejectDrop);
    EXPECT_EQ(filtered.second, merkle::objectHash(objectHashOf(*tree, CanonPath::root, rejectDrop).root));
    EXPECT_NE(filtered.second, merkle::objectHash(*tree->knownRoot));
    auto graft = namedTree("graft");
    auto mounted = makeMountedSourceAccessor({{CanonPath::root, tree}, {CanonPath("keep"), graft}});
    EXPECT_FALSE(mounted->getKnownEntry(CanonPath::root));
    auto result = fetchToStore2(settings, *store, SourcePath(mounted), FetchMode::DryRun, "grafted");
    EXPECT_EQ(result.second, merkle::objectHash(objectHashOf(*mounted, CanonPath::root).root));
}

/* The second route of 01 §9.10 through `fetchToStore2`: a copy into a
   local store under the git method is made from the store's own objects
   wherever the naming's memo names a directory the store holds, and reads
   the source only elsewhere; the path is the one the dry run named, its
   bytes the one route's, its files the blobs' links.  The memo's own
   window is the route's: what the memo names and the store holds is
   copied as named. */
TEST_F(FetchToStoreLocalTest, copyIsMadeFromTheObjectStoreWhereTheMemoNamesWhatItHolds)
{
    fetchers::Settings settings;
    ASSERT_TRUE(store->materialisesFromObjects());
    auto realPath = [&](const StorePath & p) { return store->toRealPath(p); };
    auto ino = [](const std::filesystem::path & p) { return lstat(p).st_ino; };

    /* The first tree: named, then copied.  Nothing is held yet, so every
       directory is read; the copy consults the memo per directory and
       never per file. */
    auto t1 = dirtyTree("two", "fetch-to-store-test-d-v1");
    auto [dry1, hash1] = fetchToStore2(settings, *store, SourcePath(t1), FetchMode::DryRun, "src");
    t1->reads = 0;
    auto [p1, h1] = fetchToStore2(settings, *store, SourcePath(t1), FetchMode::Copy, "src");
    EXPECT_EQ(p1, dry1);
    EXPECT_EQ(h1, hash1);
    EXPECT_EQ(t1->reads, 5u);
    ASSERT_TRUE(store->isValidPath(p1));
    EXPECT_EQ(narOf(*makeFSSourceAccessor(realPath(p1))), narOf(*t1));

    /* The second tree, one file changed in `d`: the dry run names it
       (reading `d`, answering `e` from the memo); the copy reads the root's
       file and `d`'s two and makes `e` from the objects, its files the
       first path's inodes. */
    auto t2 = dirtyTree("TWO", "fetch-to-store-test-d-v2");
    auto [dry2, hash2] = fetchToStore2(settings, *store, SourcePath(t2), FetchMode::DryRun, "src");
    EXPECT_NE(dry2, dry1);
    EXPECT_EQ(t2->reads, 3u); /* a, d/b, d/c: e from the memo */
    t2->reads = 0;
    auto [p2, h2] = fetchToStore2(settings, *store, SourcePath(t2), FetchMode::Copy, "src");
    EXPECT_EQ(p2, dry2);
    EXPECT_EQ(h2, hash2);
    EXPECT_EQ(t2->reads, 3u); /* a, d/b, d/c: e from the object store */
    ASSERT_TRUE(store->isValidPath(p2));
    auto nar2 = narOf(*t2);
    EXPECT_EQ(narOf(*makeFSSourceAccessor(realPath(p2))), nar2);
    auto info2 = store->queryPathInfo(p2);
    EXPECT_EQ(info2->narSize, nar2.size());
    ASSERT_TRUE(info2->objectHash);
    EXPECT_EQ(info2->objectHash->hash, hash2);
    for (auto & rel : {"a", "d/c", "e/x", "e/y"})
        EXPECT_EQ(ino(realPath(p2) / rel), ino(realPath(p1) / rel)) << rel;
    /* And it is the *blob's* file that was linked, not an inode that
       happens to match: a regular-file blob's canonical file is a store
       path's own -- here the first path's `e/x` -- and the index is what
       says so (`06-holder-index.md` section 2).  Before, this named the
       blob's file under `.objects/blobs`, which no regular-file blob has
       any more. */
    {
        auto holder = store->objects.holderOrForget(merkle::blobId("four"), false, NoRepair);
        ASSERT_TRUE(holder.has_value());
        EXPECT_EQ(holder->file, (realPath(p1) / "e/x").string());
        EXPECT_EQ(ino(realPath(p2) / "e/x"), ino(std::filesystem::path(holder->file)));
    }
    EXPECT_EQ(readFile(realPath(p2) / "d/b"), "TWO");

    /* An explicit filter on the tree: the memo answers no directory under
       a filter, so the route is the one route's walk with the filter
       applied; the path is the filtered tree's. */
    PathFilter dropY = [](const std::string & path) { return !hasSuffix(path, "/e/y"); };
    auto t3 = dirtyTree("TWO", "fetch-to-store-test-d-v2");
    auto [p3, h3] =
        fetchToStore2(settings, *store, SourcePath(t3), FetchMode::Copy, "src", ContentAddressMethod::Raw::Git, &dropY);
    auto [expected3, hash3] = store->computeStorePath(
        "src", SourcePath(t3), ContentAddressMethod::Raw::Git, HashAlgorithm::SHA256, {}, dropY);
    EXPECT_EQ(p3, expected3);
    EXPECT_EQ(h3, hash3);
    EXPECT_EQ(narOf(*makeFSSourceAccessor(realPath(p3))), narOf(*t3, CanonPath::root, dropY));
    EXPECT_FALSE(pathExists(realPath(p3) / "e/y"));

    /* The window, stated (01 §2.2): a change inside a subtree the memo
       still names -- here `e/y`, its name kept -- is not seen by the copy,
       which makes `e` as the memo names it; the naming would have said the
       same.  The path is the one named, and its bytes are that tree's. */
    auto t4 = dirtyTree("TWO", "fetch-to-store-test-d-v3");
    auto [dry4, hash4] = fetchToStore2(settings, *store, SourcePath(t4), FetchMode::DryRun, "src");
    t4->addFile(CanonPath("e/y"), "FIVE");
    auto [p4, h4] = fetchToStore2(settings, *store, SourcePath(t4), FetchMode::Copy, "src");
    EXPECT_EQ(p4, dry4);
    EXPECT_EQ(readFile(realPath(p4) / "e/y"), "five");
    EXPECT_EQ(narOf(*makeFSSourceAccessor(realPath(p4))), narOf(*dirtyTree("TWO", "")));
}

TEST_F(FetchToStoreTest, aBareRootIsAddressedByTheObjectHash)
{
    auto store = openStore("dummy://?read-only=false");
    /* No setting: the git name is the only name (04 section 1.9, "Step
       22's representation"). */
    fetchers::Settings settings;
    auto git = ContentAddressMethod::Raw::Git;

    auto mk = [](auto fill) {
        auto acc = make_ref<MemorySourceAccessor>();
        MemorySink sink{*acc};
        fill(sink);
        return acc;
    };
    auto plain = mk(
        [](MemorySink & s) { s.createRegularFile(CanonPath::root, [](CreateRegularFileSink & f) { f("hello\n"); }); });
    auto exec = mk([](MemorySink & s) {
        s.createRegularFile(CanonPath::root, [](CreateRegularFileSink & f) {
            f.isExecutable();
            f("hello\n");
        });
    });
    auto link = mk([](MemorySink & s) { s.createSymlink(CanonPath::root, "hello"); });
    auto dir = mk([](MemorySink & s) {
        s.createDirectory(CanonPath::root);
        s.createRegularFile(CanonPath("run"), [](CreateRegularFileSink & f) {
            f.isExecutable();
            f("hello\n");
        });
    });

    /* The executable root under the default: its hash is the object hash
       of its entry, and its path is not the plain file's and not the
       symlink's. */
    auto [pExec, hExec] = fetchToStore2(settings, *store, SourcePath(exec), FetchMode::DryRun, "x");
    EXPECT_EQ(hExec, merkle::objectHash(objectHashOf(*exec, CanonPath::root).root));
    auto [pPlain, hPlain] = fetchToStore2(settings, *store, SourcePath(plain), FetchMode::DryRun, "x");
    auto [pLink, hLink] = fetchToStore2(settings, *store, SourcePath(link), FetchMode::DryRun, "x");
    EXPECT_NE(pExec, pPlain);
    EXPECT_NE(pExec, pLink);
    EXPECT_NE(pPlain, pLink);
    /* The default is the git method: the path's content address says so. */
    EXPECT_EQ(pExec, store->makeFixedOutputPathFromCA("x", ContentAddressWithReferences::fromParts(git, hExec, {})));

    /* The plain file's object hash is its blob id: git's own identifier
       wherever git has one. */
    auto ePlain = objectHashOf(*plain, CanonPath::root).root;
    EXPECT_EQ(ePlain.mode, git::Mode::Regular);
    EXPECT_EQ(hPlain, ePlain.hash);
    EXPECT_EQ(hPlain, merkle::objectHash(ePlain));

    /* The git method asked for by name is no longer refused for an
       executable root, and gives the path the default does. */
    auto [pByName, hByName] = fetchToStore2(settings, *store, SourcePath(exec), FetchMode::DryRun, "x", git);
    EXPECT_EQ(pByName, pExec);
    EXPECT_EQ(hByName, hExec);

    /* A directory takes its tree id, as before. */
    auto [pDir, hDir] = fetchToStore2(settings, *store, SourcePath(dir), FetchMode::DryRun, "x");
    EXPECT_EQ(hDir, objectHashOf(*dir, CanonPath::root).root.hash);
}

/* The memo's rule over generated trees with a random root kind (01 section
   9.9): the row written for a node holds that node's *entry* -- the mode
   and the hash a parent's tree carries on the edge to it -- and never the
   object hash, which for a bare executable or symlink root is the
   synthetic tree's id instead.  So: name every node of a random tree
   alone, and its row must be `objectHashOf` of that node, entry for entry;
   and afterwards name each directory, whose walk now reads those rows, and
   its hash must still be the unmemoised one.  The example form is
   `GitUtilsTest.memoRowsHoldEntryHashesForBareExecutableAndSymlinkRoots`,
   which has one executable and one symlink at a repository's top level;
   here the kind is drawn, and the nodes are at every depth.

   `ContentNamedAccessor` fingerprints every node by the hash of its own
   NAR, so every node is named and every node gets a row; the three leaf
   kinds have three NARs, so the three are three keys. */
RC_GTEST_FIXTURE_PROP(FetchToStoreTest, prop_memoRowIsTheEntryHashForEveryRootKind, ())
{
    FileGenSpec spec{
        .names = {"a", "b", "d"},
        .contents = rc::gen::arbitrary<std::string>(),
        .targets = rc::gen::arbitrary<std::string>(),
    };
    auto mem = make_ref<MemorySourceAccessor>();
    mem->root = *genFile(2, spec);
    auto acc = make_ref<ContentNamedAccessor>(mem).cast<SourceAccessor>();

    auto store = openStore("dummy://?read-only=false");
    fetchers::Settings settings;
    auto git = ContentAddressMethod::Raw::Git;

    /* Every node, deepest last, so that a directory is named after its
       children's rows are written. */
    std::vector<CanonPath> nodes;
    std::function<void(const CanonPath &)> walk = [&](const CanonPath & p) {
        if (acc->lstat(p).type == SourceAccessor::tDirectory)
            for (auto & [name, _] : acc->readDirectory(p))
                walk(p / name);
        nodes.push_back(p);
    };
    walk(CanonPath::root);
    RC_ASSERT(!nodes.empty());

    unsigned directories = 0, executables = 0, symlinks = 0, plain = 0;

    for (auto & node : nodes) {
        auto entry = objectHashOf(*acc, node).root;
        switch (entry.mode) {
        case merkle::Mode::Directory:
            directories++;
            break;
        case merkle::Mode::Executable:
            executables++;
            break;
        case merkle::Mode::Symlink:
            symlinks++;
            break;
        case merkle::Mode::Regular:
            plain++;
            break;
        }

        /* Named alone: addressed by its object hash. */
        auto [path, hash] = fetchToStore2(settings, *store, SourcePath(acc, node), FetchMode::DryRun, "x");
        RC_ASSERT(hash == merkle::objectHash(entry));
        RC_ASSERT(path == gitTreePath(*store, "x", merkle::objectHash(entry)));

        /* Its row holds the entry, not the address. */
        auto [subpath, fingerprint] = acc->getFingerprint(node);
        RC_ASSERT(fingerprint.has_value());
        auto row = settings.getCache()->lookup(makeSourcePathToHashCacheKey(*fingerprint, git, subpath));
        RC_ASSERT(row.has_value());
        RC_ASSERT(Hash::parseSRI(fetchers::getStrAttr(*row, "hash")) == entry.hash);
        RC_ASSERT(fetchers::getIntAttr(*row, "mode") == uint64_t(entry.mode));
    }

    /* Every directory named again, its children now answered from their
       rows: the tree hash is still the walk's. */
    for (auto & node : nodes) {
        auto entry = objectHashOf(*acc, node).root;
        if (entry.mode != merkle::Mode::Directory)
            continue;
        auto [path, hash] = fetchToStore2(settings, *store, SourcePath(acc, node), FetchMode::DryRun, "src");
        RC_ASSERT(hash == entry.hash);
        RC_ASSERT(path == gitTreePath(*store, "src", entry.hash));
    }

    /* The generator reaches the kinds this is about; over a run of cases
       each is drawn, which the tag below records. */
    RC_TAG(
        directories > 0 ? "a directory" : "",
        executables > 0 ? "an executable" : "",
        symlinks > 0 ? "a symlink" : "",
        plain > 0 ? "a plain file" : "");
}

#ifndef _WIN32
TEST_F(FetchToStoreLocalTest, filtersKeepTheirCallerThreadAndDepthFirstEffects)
{
    Finally restoreWorkers{
        [old = LocalStore::copyWorkersForTest.load()] { LocalStore::copyWorkersForTest.store(old); }};
    Finally restoreHashWorkers{
        [old = HashingVisitor::workersForTest.load()] { HashingVisitor::workersForTest.store(old); }};
    const auto caller = std::this_thread::get_id();
    size_t cases = 0;
    for (size_t workers = 1; workers <= 8; ++workers)
        for (bool named : {false, true})
            for (auto mode : {FetchMode::DryRun, FetchMode::Copy})
                for (bool prune : {false, true}) {
                    auto reference = namedTree(std::nullopt);
                    std::vector<std::string> referenceCalls;
                    PathFilter referenceFilter = [&](const std::string & path) {
                        referenceCalls.push_back(path);
                        return !prune || !hasSuffix(path, "/drop");
                    };
                    auto referenceNar = narOf(*reference, CanonPath::root, referenceFilter);
                    auto expectedOrder =
                        prune ? std::vector<std::string>{"/drop", "/keep", "/keep/drop", "/keep/x", "/top"}
                              : std::vector<std::string>{
                                    "/drop", "/drop/z", "/keep", "/keep/drop", "/keep/drop/y", "/keep/x", "/top"};
                    EXPECT_EQ(referenceCalls, expectedOrder);
                    StringSource referenceSource{referenceNar};
                    auto expectedHash = merkle::objectHash(objectHashOfNar(referenceSource).root);

                    /* Every throwing position, plus normal completion. */
                    for (size_t stop = 0; stop <= referenceCalls.size(); ++stop) {
                        SCOPED_TRACE(
                            fmt("workers=%d named=%d copy=%d prune=%d stop=%d",
                                workers,
                                named,
                                mode == FetchMode::Copy,
                                prune,
                                stop));
                        auto tag = fmt("filter-effects-%d", cases++);
                        auto source = make_ref<ConcurrentMemorySource>();
                        source->root = namedTree(std::nullopt)->root;
                        source->fingerprint = named ? std::optional<std::string>(tag) : std::nullopt;
                        std::mutex mutex;
                        std::vector<std::string> calls;
                        bool wrongThread = false;
                        PathFilter filter = [&](const std::string & path) {
                            std::lock_guard lock(mutex);
                            wrongThread |= std::this_thread::get_id() != caller;
                            calls.push_back(path);
                            if (calls.size() - 1 == stop)
                                throw Error("filter stopped");
                            return !prune || !hasSuffix(path, "/drop");
                        };
                        LocalStore::copyWorkersForTest.store(workers);
                        HashingVisitor::workersForTest.store(workers);
                        fetchers::Settings settings;
                        auto fetch = [&] {
                            return fetchToStore2(settings, *store, SourcePath(source), mode, tag, {}, &filter);
                        };
                        if (stop < referenceCalls.size())
                            EXPECT_THROW(fetch(), Error);
                        else {
                            auto [path, hash] = fetch();
                            EXPECT_EQ(hash, expectedHash);
                            if (mode == FetchMode::Copy)
                                EXPECT_EQ(narOf(*store->requireStoreObjectAccessor(path)), referenceNar);
                        }
                        auto expectedCalls = referenceCalls;
                        if (stop < referenceCalls.size())
                            expectedCalls.resize(stop + 1);
                        EXPECT_EQ(calls, expectedCalls);
                        EXPECT_FALSE(wrongThread);
                    }
                }
    EXPECT_EQ(cases, 448u);
}

TEST_F(FetchToStoreLocalTest, copiedIdentityFiltersPreserveTheTreeAtEveryWidth)
{
    Finally restoreWorkers{
        [old = LocalStore::copyWorkersForTest.load()] { LocalStore::copyWorkersForTest.store(old); }};
    auto source = make_ref<ConcurrentMemorySource>();
    source->root = namedTree(std::nullopt)->root;
    auto expected = narOf(*source);
    for (size_t workers = 1; workers <= 8; ++workers) {
        SCOPED_TRACE(workers);
        LocalStore::copyWorkersForTest.store(workers);
        PathFilter copied = defaultPathFilter;
        auto path = store->materialise(
            fmt("identity-copy-%d", workers),
            SourcePath(source),
            copied,
            NoRepair,
            [](const CanonPath &, const SourceAccessor::Stat &) { return std::nullopt; },
            /*callbacksPermitParallelism=*/true);
        EXPECT_EQ(narOf(*store->requireStoreObjectAccessor(path)), expected);
    }
}
#endif

TEST(ScopedFilter, nestedScopesPreservePolicyCoordinatesSnapshotsAndInnerOperations)
{
    struct Inner : MemorySourceAccessor
    {
        std::vector<CanonPath> stats, reads;
        size_t active = 0, opened = 0;

        std::optional<Stat> maybeLstat(const CanonPath & path) override
        {
            EXPECT_GT(active, 0u);
            stats.push_back(path);
            return MemorySourceAccessor::maybeLstat(path);
        }

        void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            EXPECT_GT(active, 0u);
            reads.push_back(path);
            MemorySourceAccessor::readFile(path, sink, std::move(sizeCallback));
        }

        void readDirectoryWithEntries(
            const CanonPath & path,
            std::function<void(SourceAccessor &, const CanonPath &, const DirEntries &)> callback) override
        {
            ++active;
            ++opened;
            Finally close{[&] { --active; }};
            SourceAccessor::readDirectoryWithEntries(path, std::move(callback));
        }
    };

    struct Policy : FilteringSourceAccessor
    {
        void anchor() override {}

        unsigned outcome = 0;
        std::optional<CanonPath> initiallyDenied;
        std::optional<CanonPath> throwsAt;
        std::vector<CanonPath> admissions, names, errors;

        Policy(const SourcePath & source)
            : FilteringSourceAccessor(source, [this](const CanonPath & path) {
                errors.push_back(path);
                return RestrictedPathError("denied '%s'", path);
            })
        {
        }

        bool isAllowed(const CanonPath & path) override
        {
            admissions.push_back(path);
            if (outcome == 2 || throwsAt == path)
                throw Error("admission failed");
            return outcome == 0 && initiallyDenied != path;
        }

        std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
        {
            names.push_back(path);
            if (outcome == 2)
                throw Error("naming failed");
            /* A custom policy's naming operation must survive scoping;
               it is not interchangeable with the inner accessor's. */
            return {path, outcome == 1 ? std::optional("policy:" + path.abs()) : std::nullopt};
        }

        void readDirectoryWithEntries(
            const CanonPath & path,
            std::function<void(SourceAccessor &, const CanonPath &, const DirEntries &)> callback) override
        {
            readDirectoryScoped(path, std::move(callback));
        }
    };

    auto inner = make_ref<Inner>();
    const CanonPath physical("physical");
    for (auto initial : {CanonPath::root, CanonPath("start")}) {
        auto at = physical / initial;
        for (unsigned depth = 0; depth <= 3; ++depth) {
            for (auto base : {at, at / "deep"}) {
                inner->addFile(base / "plain", "bytes:" + (base / "plain").abs());
                MemorySink sink{*inner};
                sink.createRegularFile(base / "exec", [&](CreateRegularFileSink & file) {
                    file.isExecutable();
                    file("bytes:" + (base / "exec").abs());
                });
                sink.createDirectory(base / "dir");
                sink.createSymlink(base / "link", "target");
            }
            at.push("a");
        }
    }

    std::vector<CanonPath> queries{CanonPath::root};
    for (auto base : {CanonPath::root, CanonPath("deep")})
        for (auto name : {"plain", "exec", "dir", "link", "missing"})
            queries.push_back(base / name);

    size_t cases = 0;
    /* Every sequence of root/child scopes up to three nested scopes,
       both initial coordinate origins, every represented node kind and
       absence at both query depths, both initial admissions and all later
       policy outcomes. The scope root was already admitted. */
    for (auto initial : {CanonPath::root, CanonPath("start")})
        for (unsigned depth = 0; depth <= 3; ++depth)
            for (unsigned steps = 0; steps < (1u << depth); ++steps)
                for (const auto & query : queries)
                    for (bool initiallyAllowed : {false, true})
                        for (unsigned outcome = 0; outcome < 3; ++outcome) {
                            if (query.isRoot() && !initiallyAllowed)
                                continue;
                            SCOPED_TRACE(
                                fmt("initial=%s depth=%d steps=%d query=%s initiallyAllowed=%d outcome=%d",
                                    initial.abs(),
                                    depth,
                                    steps,
                                    query.abs(),
                                    initiallyAllowed,
                                    outcome));
                            Policy policy{SourcePath(inner, physical)};
                            auto final = initial;
                            for (unsigned level = 0; level < depth; ++level)
                                if (steps & (1u << level))
                                    final.push("a");
                            if (!initiallyAllowed)
                                policy.initiallyDenied = final / query;
                            inner->opened = 0;
                            std::vector<CanonPath> expectedAdmissions{initial};
                            auto listed = [&](const CanonPath & at) {
                                auto entries = inner->MemorySourceAccessor::readDirectory(physical / at);
                                for (auto & [name, type] : entries)
                                    expectedAdmissions.push_back(at / name);
                            };
                            listed(initial);
                            auto inspect = [&](this const auto & descend,
                                               SourceAccessor & scoped,
                                               const CanonPath & relative,
                                               const CanonPath & at,
                                               unsigned level) -> void {
                                EXPECT_EQ(relative, CanonPath::root);
                                /* Once only root re-scopes remain, change the
                                   policy before entering them. Each must retain
                                   the already captured immediate-child set. */
                                if ((steps >> level) == 0) {
                                    policy.initiallyDenied.reset();
                                    policy.outcome = outcome;
                                }
                                if (level < depth) {
                                    CanonPath step{(steps & (1u << level)) ? "a" : "/"};
                                    auto child = at / step;
                                    /* A root scope reuses its admission snapshot;
                                       a deeper listing asks the policy once per entry. */
                                    if (!step.isRoot())
                                        listed(child);
                                    scoped.readDirectoryWithEntries(
                                        step, [&](SourceAccessor & next, const CanonPath & rel, const auto &) {
                                            descend(next, rel, child, level + 1);
                                        });
                                    return;
                                }
                                EXPECT_EQ(policy.admissions, expectedAdmissions);
                                policy.admissions.clear();
                                policy.outcome = outcome;
                                inner->stats.clear();
                                inner->reads.clear();
                                auto full = at / query;
                                auto physicalPath = physical / full;
                                auto expected = inner->MemorySourceAccessor::maybeLstat(physicalPath);
                                bool immediate = !query.isRoot() && query.parent()->isRoot();
                                bool consultsPolicy = !query.isRoot() && !immediate;
                                bool admitted =
                                    query.isRoot()
                                    || (immediate ? initiallyAllowed && expected.has_value() : outcome == 0);
                                if (consultsPolicy && outcome == 2)
                                    EXPECT_THROW(scoped.maybeLstat(query), Error);
                                else {
                                    auto actual = scoped.maybeLstat(query);
                                    EXPECT_EQ(actual.has_value(), admitted && expected.has_value());
                                    if (actual && expected) {
                                        EXPECT_EQ(actual->type, expected->type);
                                        EXPECT_EQ(actual->isExecutable, expected->isExecutable);
                                        EXPECT_EQ(actual->fileSize, expected->fileSize);
                                    }
                                }
                                EXPECT_EQ(
                                    policy.admissions, consultsPolicy ? std::vector{full} : std::vector<CanonPath>{});
                                EXPECT_EQ(
                                    inner->stats, admitted ? std::vector{physicalPath} : std::vector<CanonPath>{});

                                policy.admissions.clear();
                                if (admitted && expected && expected->type == SourceAccessor::tRegular) {
                                    EXPECT_EQ(scoped.readFile(query), "bytes:" + physicalPath.abs());
                                    EXPECT_EQ(inner->reads, std::vector{physicalPath});
                                    EXPECT_EQ(
                                        policy.admissions,
                                        consultsPolicy ? std::vector{full} : std::vector<CanonPath>{});
                                } else if (!admitted && !(consultsPolicy && outcome == 2)) {
                                    EXPECT_THROW(scoped.readFile(query), RestrictedPathError);
                                    EXPECT_EQ(policy.errors, std::vector{full});
                                    EXPECT_TRUE(inner->reads.empty());
                                    EXPECT_EQ(
                                        policy.admissions,
                                        consultsPolicy ? std::vector{full} : std::vector<CanonPath>{});
                                }

                                if (outcome == 2)
                                    EXPECT_THROW(scoped.getFingerprint(query), Error);
                                else {
                                    auto named = scoped.getFingerprint(query);
                                    EXPECT_EQ(named.first, full);
                                    EXPECT_EQ(
                                        named.second,
                                        outcome == 1 ? std::optional("policy:" + full.abs()) : std::nullopt);
                                }
                                EXPECT_EQ(policy.names, std::vector{full});
                            };
                            policy.readDirectoryWithEntries(
                                initial, [&](SourceAccessor & scoped, const CanonPath & relative, const auto &) {
                                    inspect(scoped, relative, initial, 0);
                                });
                            EXPECT_EQ(inner->active, 0u);
                            EXPECT_EQ(inner->opened, depth + 1);
                            ++cases;
                        }
    EXPECT_EQ(cases, 1890u);

    /* These exceptions leave the callback stack, unlike the operation
       checks above: every opened inner scope must unwind with them. */
    enum class Failure { RootAdmission, ChildAdmission, DeepAdmission, Naming, Callback };
    size_t escaped = 0;
    for (auto failure :
         {Failure::RootAdmission, Failure::ChildAdmission, Failure::DeepAdmission, Failure::Naming, Failure::Callback})
        for (unsigned depth = 0; depth <= 3; ++depth) {
            SCOPED_TRACE(fmt("failure=%d depth=%d", static_cast<unsigned>(failure), depth));
            Policy policy{SourcePath(inner, physical)};
            const CanonPath initial("start");
            inner->opened = 0;
            bool callbackEntered = false;
            if (failure == Failure::RootAdmission)
                policy.throwsAt = initial;
            else if (failure == Failure::ChildAdmission)
                policy.throwsAt = initial / "plain";
            auto descend = [&](this const auto & self, SourceAccessor & scoped, unsigned level) -> void {
                if (level < depth) {
                    scoped.readDirectoryWithEntries(
                        CanonPath("a"), [&](auto & next, const auto &, const auto &) { self(next, level + 1); });
                    return;
                }
                if (failure == Failure::DeepAdmission) {
                    policy.outcome = 2;
                    scoped.maybeLstat(CanonPath("deep/plain"));
                } else if (failure == Failure::Naming) {
                    policy.outcome = 2;
                    scoped.getFingerprint(CanonPath::root);
                } else
                    throw Error("callback failed");
            };
            EXPECT_THROW(
                policy.readDirectoryWithEntries(
                    initial,
                    [&](auto & scoped, const auto &, const auto &) {
                        callbackEntered = true;
                        descend(scoped, 0);
                    }),
                Error);
            EXPECT_EQ(inner->active, 0u);
            EXPECT_EQ(callbackEntered, failure != Failure::RootAdmission && failure != Failure::ChildAdmission);
            EXPECT_EQ(
                inner->opened,
                failure == Failure::RootAdmission    ? 0u
                : failure == Failure::ChildAdmission ? 1u
                                                     : depth + 1);
            ++escaped;
        }
    EXPECT_EQ(escaped, 20u);
}

#ifndef _WIN32
TEST(ScopedFilter, nestedScopesKeepTheListedPhysicalDirectoryAfterRename)
{
    AutoDelete directory{createTempDir()};
    auto before = directory.path() / "before";
    auto moved = directory.path() / "moved";
    createDirs(before / "child");
    writeFile(before / "child/file", "observed");
    createSymlink("file", before / "child/link");
    auto accessor = AllowListSourceAccessor::create(
        makeFSSourceAccessor(directory.path()), {CanonPath("before")}, {}, [](const CanonPath & path) {
            return RestrictedPathError("denied '%s'", path);
        });
    accessor->readDirectoryWithEntries(CanonPath("before"), [&](auto & outer, const auto &, const auto &) {
        outer.readDirectoryWithEntries(CanonPath("child"), [&](auto & inner, const auto &, const auto & entries) {
            ASSERT_EQ(entries.size(), 2u);
            std::filesystem::rename(before, moved);
            createDirs(before / "child");
            writeFile(before / "child/file", "replacement");
            EXPECT_EQ(inner.readFile(CanonPath("file")), "observed");
            EXPECT_EQ(inner.readLink(CanonPath("link")), "file");
            inner.readDirectoryWithEntries(CanonPath::root, [&](auto & repeated, const auto &, const auto &) {
                EXPECT_EQ(repeated.readFile(CanonPath("file")), "observed");
                EXPECT_THROW(
                    repeated.readDirectoryWithEntries(CanonPath("link"), [](auto &, const auto &, const auto &) {}),
                    SymlinkNotAllowed);
            });
        });
    });
    EXPECT_EQ(readFile(before / "child/file"), "replacement");
}
#endif

TEST(ScopedFilter, concurrencyPermissionRequiresBothThePolicyAndPhysicalAccessor)
{
    struct Inner final : MemorySourceAccessor
    {
        bool permission = false;

        bool allowsConcurrentTraversal() override
        {
            return permission;
        }
    };

    struct Policy final : FilteringSourceAccessor
    {
        void anchor() override {}

        bool permission;

        Policy(ref<SourceAccessor> inner, bool permission)
            : FilteringSourceAccessor(
                  SourcePath(inner), [](const CanonPath &) { return RestrictedPathError("denied"); })
            , permission(permission)
        {
        }

        bool isAllowed(const CanonPath &) override
        {
            return true;
        }

        bool allowsConcurrentTraversal() override
        {
            return permission;
        }

        void readDirectoryWithEntries(
            const CanonPath & path,
            std::function<void(SourceAccessor &, const CanonPath &, const DirEntries &)> callback) override
        {
            readDirectoryScoped(path, std::move(callback));
        }
    };

    size_t cases = 0;
    for (bool physical : {false, true})
        for (bool policy : {false, true}) {
            auto inner = make_ref<Inner>();
            inner->permission = physical;
            inner->addFile(CanonPath("a/a/a/file"), "bytes");
            Policy owner{inner, policy};
            auto inspect = [&](this const auto & recurse,
                               SourceAccessor & accessor,
                               const CanonPath & path,
                               unsigned depth) -> void {
                accessor.readDirectoryWithEntries(
                    path, [&](SourceAccessor & scoped, const CanonPath & relative, const auto &) {
                        SCOPED_TRACE(fmt("physical=%d policy=%d depth=%d", physical, policy, depth));
                        EXPECT_EQ(scoped.allowsConcurrentTraversal(), physical && policy);
                        ++cases;
                        if (depth < 3)
                            recurse(scoped, relative / "a", depth + 1);
                    });
            };
            inspect(owner, CanonPath::root, 0);
            auto allowed = AllowListSourceAccessor::create(
                inner, {CanonPath::root}, {}, [](const CanonPath &) { return RestrictedPathError("denied"); });
            EXPECT_EQ(allowed->allowsConcurrentTraversal(), physical);
        }
    EXPECT_EQ(cases, 16u);
}

TEST_F(FetchToStoreTest, parallelMemoKeepsEachNamedFactWithItsNode)
{
    struct Source final : MemorySourceAccessor
    {
        std::map<CanonPath, std::string> names;
        std::mutex mutex;
        std::map<CanonPath, size_t> observations, reads;
        bool symlinkNamedBeforeRead = false;

        bool allowsConcurrentTraversal() override
        {
            return true;
        }

        std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
        {
            std::lock_guard lock(mutex);
            ++observations[path];
            auto i = names.find(path);
            return {CanonPath::root, i == names.end() ? std::nullopt : std::optional(i->second)};
        }

        void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            {
                std::lock_guard lock(mutex);
                ++reads[path];
            }
            MemorySourceAccessor::readFile(path, sink, std::move(sizeCallback));
        }

        std::string readLink(const CanonPath & path) override
        {
            {
                std::lock_guard lock(mutex);
                symlinkNamedBeforeRead |= observations[path] != 0;
                ++reads[path];
            }
            return MemorySourceAccessor::readLink(path);
        }
    };

    auto store = openStore("dummy://?read-only=false");
    auto cacheHome = getEnv("NIX_CACHE_HOME").value();
    auto unavailable = createTempDir();
    AutoDelete cleanup(unavailable, true);
    auto blocked = unavailable / "file";
    writeFile(blocked, "not a directory");
    Finally restoreCache{[&] { setEnv("NIX_CACHE_HOME", cacheHome.c_str()); }};
    Finally restoreWorkers{
        [old = HashingVisitor::workersForTest.load()] { HashingVisitor::workersForTest.store(old); }};
    size_t cases = 0;
    for (size_t workers = 1; workers <= 8; ++workers)
        for (bool available : {false, true})
            for (unsigned named = 0; named < 5; ++named) {
                bool hit = named == 2 || named == 4;
                if (!available && hit)
                    continue; // A hit requires an available cache.
                SCOPED_TRACE(fmt("workers=%d available=%d named=%d", workers, available, named));
                setEnv("NIX_CACHE_HOME", (available ? cacheHome : blocked.string()).c_str());
                fetchers::Settings settings;
                auto source = make_ref<Source>();
                source->addFile(CanonPath("a/plain"), "plain");
                source->addFile(CanonPath("b/plain"), "different");
                MemorySink sink{*source};
                sink.createRegularFile(CanonPath("a/exec"), [](CreateRegularFileSink & file) {
                    file.isExecutable();
                    file("executable");
                });
                sink.createSymlink(CanonPath("b/link"), "a/plain");
                const std::vector<CanonPath> leaves{
                    CanonPath("a/plain"), CanonPath("a/exec"), CanonPath("b/plain"), CanonPath("b/link")};
                auto nodes = leaves;
                nodes.insert(nodes.end(), {CanonPath("a"), CanonPath("b")});
                std::map<CanonPath, merkle::TreeEntry> entries;
                for (auto & path : nodes)
                    entries.emplace(path, referenceObjectEntry(*source, path));
                auto expected = referenceObjectEntry(*source, CanonPath::root);
                source->observations.clear();
                source->reads.clear();
                source->symlinkNamedBeforeRead = false;
                if (named)
                    for (auto & path : nodes) {
                        if (named < 3 && entries.at(path).mode == merkle::Mode::Directory)
                            continue;
                        auto name = fmt("parallel-memo-%d-%s", cases, path);
                        source->names.emplace(path, name);
                        if (hit) {
                            auto & entry = entries.at(path);
                            settings.getCache()->upsert(
                                makeSourcePathToHashCacheKey(name, ContentAddressMethod::Raw::Git, CanonPath::root),
                                {{"hash", entry.hash.to_string(HashFormat::SRI, true)},
                                 {"mode", uint64_t(entry.mode)}});
                        }
                    }
                HashingVisitor::workersForTest.store(workers);
                auto actual = fetchToStore2(settings, *store, SourcePath(source), FetchMode::DryRun, "source");
                EXPECT_EQ(actual.second, merkle::objectHash(expected));
                EXPECT_EQ(source->observations[CanonPath::root], 1u);
                EXPECT_EQ(source->observations[CanonPath("a")], 1u);
                EXPECT_EQ(source->observations[CanonPath("b")], 1u);
                EXPECT_FALSE(source->symlinkNamedBeforeRead);
                for (auto & path : leaves) {
                    EXPECT_EQ(source->observations[path], named == 4 ? 0u : 1u);
                    EXPECT_EQ(source->reads[path], named == 4 || (hit && path != CanonPath("b/link")) ? 0u : 1u);
                }
                if (named && available)
                    for (auto & [path, name] : source->names) {
                        auto row = settings.getCache()->lookup(
                            makeSourcePathToHashCacheKey(name, ContentAddressMethod::Raw::Git, CanonPath::root));
                        ASSERT_TRUE(row);
                        EXPECT_EQ(Hash::parseSRI(fetchers::getStrAttr(*row, "hash")), entries.at(path).hash);
                        EXPECT_EQ(fetchers::getIntAttr(*row, "mode"), uint64_t(entries.at(path).mode));
                    }
                ++cases;
            }
    EXPECT_EQ(cases, 64u);

    /* Cross the memo's 16,384-row flush boundary with named siblings on
       different workers. Check every persisted binding, not a sample. */
    setEnv("NIX_CACHE_HOME", cacheHome.c_str());
    fetchers::Settings settings;
    auto source = make_ref<Source>();
    std::map<CanonPath, Hash> expected;
    for (size_t i = 0; i < 16385; ++i) {
        auto path = CanonPath(i % 2 ? "a" : "b") / fmt("file-%d", i);
        auto bytes = fmt("contents-%d", i);
        expected.emplace(path, merkle::blobId(bytes));
        source->addFile(path, std::move(bytes));
        source->names.emplace(path, fmt("parallel-flush-%d", i));
    }
    auto root = referenceObjectEntry(*source, CanonPath::root);
    HashingVisitor::workersForTest.store(8);
    auto actual = fetchToStore2(settings, *store, SourcePath(source), FetchMode::DryRun, "flush");
    EXPECT_EQ(actual.second, merkle::objectHash(root));
    for (auto & [path, hash] : expected) {
        SCOPED_TRACE(path.abs());
        auto row = settings.getCache()->lookup(
            makeSourcePathToHashCacheKey(source->names.at(path), ContentAddressMethod::Raw::Git, CanonPath::root));
        ASSERT_TRUE(row);
        EXPECT_EQ(Hash::parseSRI(fetchers::getStrAttr(*row, "hash")), hash);
        EXPECT_EQ(fetchers::getIntAttr(*row, "mode"), uint64_t(merkle::Mode::Regular));
    }
}

} // namespace nix
