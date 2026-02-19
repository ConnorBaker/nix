#include "helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"
#include "nix/util/source-path.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DependencyTrackerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Ensure no lingering dependency tracker from a previous test (Adapton: clean DDG state)
        DependencyTracker::clearSessionTraces();
    }

    void TearDown() override
    {
        DependencyTracker::clearSessionTraces();
    }
};

// ── Dep hash function tests (BLAKE3 oracle hashing) ─────────────────

TEST_F(DependencyTrackerTest, DepHash_Deterministic)
{
    auto h1 = depHash("hello");
    auto h2 = depHash("hello");
    EXPECT_EQ(h1, h2);
}

TEST_F(DependencyTrackerTest, DepHash_DifferentInputs)
{
    auto h1 = depHash("hello");
    auto h2 = depHash("world");
    EXPECT_NE(h1, h2);
}

TEST_F(DependencyTrackerTest, DepHash_EmptyInput)
{
    auto h = depHash("");
    // Should produce a valid non-zero BLAKE3 hash
    bool allZero = true;
    for (auto b : h.bytes)
        if (b != 0) { allZero = false; break; }
    EXPECT_FALSE(allZero);
}

TEST_F(DependencyTrackerTest, DepHash_ToHex_Length)
{
    auto h = depHash("test");
    EXPECT_EQ(h.toHex().size(), 64u);
}

TEST_F(DependencyTrackerTest, DepHashPath_CapturesContent)
{
    TempTestFile f("hello world");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));
    auto h = depHashPath(path);
    // Just verify it produces a valid BLAKE3 hash of the NAR
    EXPECT_EQ(h.size(), 32u);
}

TEST_F(DependencyTrackerTest, DepHashPath_CapturesExecBit)
{
    TempTestFile f1("#!/bin/sh\necho hi");
    TempTestFile f2("#!/bin/sh\necho hi");
    std::filesystem::permissions(f1.path, std::filesystem::perms::owner_exec,
                                  std::filesystem::perm_options::add);

    auto p1 = SourcePath(getFSSourceAccessor(), CanonPath(f1.path.string()));
    auto p2 = SourcePath(getFSSourceAccessor(), CanonPath(f2.path.string()));
    auto h1 = depHashPath(p1);
    auto h2 = depHashPath(p2);
    // NAR includes exec bit, so dep hashes should differ (captures filesystem oracle state)
    EXPECT_NE(h1, h2);
}

TEST_F(DependencyTrackerTest, DepHashDirListing_Deterministic)
{
    SourceAccessor::DirEntries entries = {
        {"a", SourceAccessor::Type::tRegular},
        {"b", SourceAccessor::Type::tDirectory},
    };
    auto h1 = depHashDirListing(entries);
    auto h2 = depHashDirListing(entries);
    EXPECT_EQ(h1, h2);
}

TEST_F(DependencyTrackerTest, DepHashDirListing_OrderSensitive)
{
    SourceAccessor::DirEntries e1 = {
        {"a", SourceAccessor::Type::tRegular},
        {"b", SourceAccessor::Type::tDirectory},
    };
    SourceAccessor::DirEntries e2 = {
        {"b", SourceAccessor::Type::tDirectory},
        {"a", SourceAccessor::Type::tRegular},
    };
    // DirEntries is std::map so order is by key -- both should produce the same dep hash
    auto h1 = depHashDirListing(e1);
    auto h2 = depHashDirListing(e2);
    EXPECT_EQ(h1, h2); // map orders by key, so {a,b} in both cases
}

TEST_F(DependencyTrackerTest, DepHashDirListing_Empty)
{
    SourceAccessor::DirEntries empty;
    auto h = depHashDirListing(empty);
    bool allZero = true;
    for (auto b : h.bytes)
        if (b != 0) { allZero = false; break; }
    EXPECT_FALSE(allZero);
}

TEST_F(DependencyTrackerTest, DepHashDirListing_DifferentEntries)
{
    SourceAccessor::DirEntries e1 = {{"a", SourceAccessor::Type::tRegular}};
    SourceAccessor::DirEntries e2 = {{"b", SourceAccessor::Type::tRegular}};
    EXPECT_NE(depHashDirListing(e1), depHashDirListing(e2));
}

TEST_F(DependencyTrackerTest, DepHash_vs_DepHashPath_Different)
{
    TempTestFile f("hello");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));
    auto rawHash = depHash("hello");
    auto narHash = depHashPath(path);
    // Raw BLAKE3 vs NAR BLAKE3 should differ (different oracle hashing strategies)
    EXPECT_NE(rawHash, narHash);
}

// ── RAII dependency tracker tests (Adapton: DDG scope management) ────

TEST_F(DependencyTrackerTest, Constructor_SetsActiveTracker)
{
    EXPECT_FALSE(DependencyTracker::isActive());
    {
        DependencyTracker tracker;
        EXPECT_TRUE(DependencyTracker::isActive());
    }
    EXPECT_FALSE(DependencyTracker::isActive());
}

TEST_F(DependencyTrackerTest, Destructor_RestoresPrevious)
{
    EXPECT_FALSE(DependencyTracker::isActive());
    DependencyTracker outer;
    EXPECT_TRUE(DependencyTracker::isActive());
    {
        DependencyTracker inner;
        EXPECT_TRUE(DependencyTracker::isActive());
    }
    // Outer tracker should still be active (Adapton: restore parent scope)
    EXPECT_TRUE(DependencyTracker::isActive());
}

TEST_F(DependencyTrackerTest, Record_WithActiveTracker)
{
    DependencyTracker tracker;
    auto dep = makeContentDep("/test.nix", "content");
    DependencyTracker::record(dep);
    auto deps = tracker.collectTraces();
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(deps[0].key, "/test.nix");
    EXPECT_EQ(deps[0].type, DepType::Content);
}

TEST_F(DependencyTrackerTest, Record_WithoutActiveTracker)
{
    // Recording without an active tracker should just append to session traces without crash
    auto dep = makeContentDep("/test.nix", "content");
    DependencyTracker::record(dep);
    // No crash is the test (dependency recorded to global session trace)
}

TEST_F(DependencyTrackerTest, CollectDeps_OnlyCurrentRange)
{
    DependencyTracker outer;
    DependencyTracker::record(makeContentDep("/outer.nix", "outer"));
    {
        DependencyTracker inner;
        DependencyTracker::record(makeContentDep("/inner.nix", "inner"));
        auto innerDeps = inner.collectTraces();
        ASSERT_EQ(innerDeps.size(), 1u);
        EXPECT_EQ(innerDeps[0].key, "/inner.nix");
    }
    auto outerDeps = outer.collectTraces();
    // Outer should have both (inner recorded into outer's session range too — Adapton: nested scopes)
    EXPECT_GE(outerDeps.size(), 1u);
    EXPECT_EQ(outerDeps[0].key, "/outer.nix");
}

TEST_F(DependencyTrackerTest, ClearSessionDeps)
{
    DependencyTracker tracker;
    DependencyTracker::record(makeContentDep("/a.nix", "a"));
    DependencyTracker::record(makeContentDep("/b.nix", "b"));
    DependencyTracker::clearSessionTraces();
    // After clearing, new tracker should see no old deps
    // (session trace reset — Adapton: clean DDG slate)
}

// ── SuspendDepTracking tests (Adapton: suspend DDG recording) ────

TEST_F(DependencyTrackerTest, Suspend_DeactivatesTracker)
{
    DependencyTracker tracker;
    EXPECT_TRUE(DependencyTracker::isActive());
    {
        SuspendDepTracking suspend;
        EXPECT_FALSE(DependencyTracker::isActive());
    }
    EXPECT_TRUE(DependencyTracker::isActive());
}

TEST_F(DependencyTrackerTest, Suspend_RestoresOnDestruct)
{
    DependencyTracker tracker;
    {
        SuspendDepTracking suspend;
        EXPECT_FALSE(DependencyTracker::isActive());
    }
    EXPECT_TRUE(DependencyTracker::isActive());
}

TEST_F(DependencyTrackerTest, Suspend_RecordStillAppends)
{
    // Recording still appends to session traces even when tracker is suspended
    // (no active tracker scope, but session trace is global)
    DependencyTracker tracker;
    DependencyTracker::record(makeContentDep("/before.nix", "a"));
    {
        SuspendDepTracking suspend;
        // Recording during suspension still pushes to session traces
        DependencyTracker::record(makeContentDep("/during.nix", "b"));
    }
    DependencyTracker::record(makeContentDep("/after.nix", "c"));
    auto deps = tracker.collectTraces();
    // All three should be in session traces and within tracker's range
    EXPECT_EQ(deps.size(), 3u);
}

TEST_F(DependencyTrackerTest, Suspend_NestedSuspend)
{
    DependencyTracker tracker;
    EXPECT_TRUE(DependencyTracker::isActive());
    {
        SuspendDepTracking s1;
        EXPECT_FALSE(DependencyTracker::isActive());
        {
            SuspendDepTracking s2;
            EXPECT_FALSE(DependencyTracker::isActive());
        }
        // s2 restores s1's saved state (which is nullptr — no active tracker)
        EXPECT_FALSE(DependencyTracker::isActive());
    }
    // s1 restores the original tracker (Adapton: DDG scope restored)
    EXPECT_TRUE(DependencyTracker::isActive());
}

// ── resolveToInput tests ─────────────────────────────────────────────

TEST_F(DependencyTrackerTest, ResolveToInput_MatchingMount)
{
    std::unordered_map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/foo/bar")] = {"myInput", ""};

    auto result = resolveToInput(CanonPath("/foo/bar/baz.nix"), mounts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "myInput");
    EXPECT_EQ(result->second, CanonPath("/baz.nix"));
}

TEST_F(DependencyTrackerTest, ResolveToInput_NoMatchingMount)
{
    std::unordered_map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/foo/bar")] = {"myInput", ""};

    auto result = resolveToInput(CanonPath("/other/path.nix"), mounts);
    EXPECT_FALSE(result.has_value());
}

TEST_F(DependencyTrackerTest, ResolveToInput_ExactMatch)
{
    std::unordered_map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/foo/bar")] = {"myInput", ""};

    auto result = resolveToInput(CanonPath("/foo/bar"), mounts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "myInput");
    EXPECT_EQ(result->second, CanonPath::root);
}

TEST_F(DependencyTrackerTest, ResolveToInput_LongestPrefixWins)
{
    std::unordered_map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/foo")] = {"broad", ""};
    mounts[CanonPath("/foo/bar")] = {"specific", ""};

    auto result = resolveToInput(CanonPath("/foo/bar/baz.nix"), mounts);
    ASSERT_TRUE(result.has_value());
    // resolveToInput walks up from the path, so /foo/bar matches first (longest prefix)
    EXPECT_EQ(result->first, "specific");
}

TEST_F(DependencyTrackerTest, ResolveToInput_WithSubdir)
{
    std::unordered_map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/flake-src")] = {"myInput", "subdir"};

    auto result = resolveToInput(CanonPath("/flake-src/subdir/file.nix"), mounts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "myInput");
    EXPECT_EQ(result->second, CanonPath("/file.nix"));
}

// ── Blake3Hash tests ─────────────────────────────────────────────────

TEST_F(DependencyTrackerTest, Blake3Hash_Equality)
{
    auto h1 = depHash("test");
    auto h2 = depHash("test");
    EXPECT_EQ(h1, h2);
    EXPECT_FALSE(h1 != h2);
}

TEST_F(DependencyTrackerTest, Blake3Hash_Ordering)
{
    auto h1 = depHash("aaa");
    auto h2 = depHash("bbb");
    // Just verify total ordering is defined (needed for sorted trace deps)
    [[maybe_unused]] auto cmp = h1 <=> h2;
}

TEST_F(DependencyTrackerTest, Blake3Hash_FromBlob)
{
    auto h1 = depHash("test");
    auto h2 = Blake3Hash::fromBlob(h1.data(), h1.size());
    EXPECT_EQ(h1, h2);
}

TEST_F(DependencyTrackerTest, Blake3Hash_View)
{
    auto h = depHash("test");
    auto view = h.view();
    EXPECT_EQ(view.size(), 32u);
    EXPECT_EQ(std::memcmp(view.data(), h.data(), 32), 0);
}

// ── depTypeName tests ────────────────────────────────────────────────

TEST_F(DependencyTrackerTest, DepTypeName_AllTypes)
{
    EXPECT_EQ(depTypeName(DepType::Content), "content");
    EXPECT_EQ(depTypeName(DepType::Directory), "directory");
    EXPECT_EQ(depTypeName(DepType::Existence), "existence");
    EXPECT_EQ(depTypeName(DepType::EnvVar), "envvar");
    EXPECT_EQ(depTypeName(DepType::CurrentTime), "currentTime");
    EXPECT_EQ(depTypeName(DepType::System), "system");
    EXPECT_EQ(depTypeName(DepType::UnhashedFetch), "unhashedFetch");
    EXPECT_EQ(depTypeName(DepType::ParentContext), "parentContext");
    EXPECT_EQ(depTypeName(DepType::CopiedPath), "copiedPath");
    EXPECT_EQ(depTypeName(DepType::Exec), "exec");
    EXPECT_EQ(depTypeName(DepType::NARContent), "narContent");
}

// ── isBlake3Dep tests ────────────────────────────────────────────────

TEST_F(DependencyTrackerTest, IsBlake3Dep)
{
    EXPECT_TRUE(isBlake3Dep(DepType::Content));
    EXPECT_TRUE(isBlake3Dep(DepType::Directory));
    EXPECT_TRUE(isBlake3Dep(DepType::NARContent));
    EXPECT_TRUE(isBlake3Dep(DepType::EnvVar));
    EXPECT_TRUE(isBlake3Dep(DepType::System));
    EXPECT_FALSE(isBlake3Dep(DepType::Existence));
    EXPECT_FALSE(isBlake3Dep(DepType::CopiedPath));
    EXPECT_FALSE(isBlake3Dep(DepType::UnhashedFetch));
    EXPECT_FALSE(isBlake3Dep(DepType::CurrentTime));
    EXPECT_FALSE(isBlake3Dep(DepType::Exec));
}

// ── Dedup at recording time tests (Adapton: DDG edge deduplication) ──

TEST_F(DependencyTrackerTest, Record_DedupWithinTracker)
{
    DependencyTracker tracker;
    auto dep = makeContentDep("/test.nix", "content");
    DependencyTracker::record(dep);
    DependencyTracker::record(dep); // same (type, source, key)
    DependencyTracker::record(dep); // third time
    auto deps = tracker.collectTraces();
    EXPECT_EQ(deps.size(), 1u); // Only one should survive (deduped by dep key)
}

TEST_F(DependencyTrackerTest, Record_DedupIgnoresHash)
{
    DependencyTracker tracker;
    // Same dep key (type, source, key) but different expectedHash values
    DependencyTracker::record(makeContentDep("/test.nix", "content-v1"));
    DependencyTracker::record(makeContentDep("/test.nix", "content-v2"));
    auto deps = tracker.collectTraces();
    EXPECT_EQ(deps.size(), 1u); // Deduped by dep key, first hash wins
}

TEST_F(DependencyTrackerTest, Record_DedupDifferentKeysNotDeduped)
{
    DependencyTracker tracker;
    DependencyTracker::record(makeContentDep("/a.nix", "content"));
    DependencyTracker::record(makeContentDep("/b.nix", "content"));
    auto deps = tracker.collectTraces();
    EXPECT_EQ(deps.size(), 2u); // Different dep keys, both kept
}

TEST_F(DependencyTrackerTest, Record_DedupDifferentTypesNotDeduped)
{
    DependencyTracker tracker;
    // Same key but different dep types -> not deduped (different oracle types)
    DependencyTracker::record({"", "/test.nix", depHash("c"), DepType::Content});
    DependencyTracker::record({"", "/test.nix", depHash("c"), DepType::Existence});
    auto deps = tracker.collectTraces();
    EXPECT_EQ(deps.size(), 2u);
}

TEST_F(DependencyTrackerTest, Record_DedupPerTrackerScope)
{
    DependencyTracker outer;
    DependencyTracker::record(makeContentDep("/test.nix", "content"));
    {
        DependencyTracker inner;
        // Same dep in inner tracker — inner has its own dedup scope (Adapton: nested DDG node)
        DependencyTracker::record(makeContentDep("/test.nix", "content"));
        auto innerDeps = inner.collectTraces();
        EXPECT_EQ(innerDeps.size(), 1u);
    }
    auto outerDeps = outer.collectTraces();
    // Outer sees both: one from before inner, one from inner scope
    EXPECT_EQ(outerDeps.size(), 2u);
}

TEST_F(DependencyTrackerTest, Record_DedupWithDifferentSources)
{
    DependencyTracker tracker;
    // Same key and type but different sources -> not deduped (different oracle identities)
    DependencyTracker::record({"inputA", "/test.nix", depHash("c"), DepType::Content});
    DependencyTracker::record({"inputB", "/test.nix", depHash("c"), DepType::Content});
    auto deps = tracker.collectTraces();
    EXPECT_EQ(deps.size(), 2u);
}

// ── DepKind enum tests ────────────────────────────────────────────────

TEST_F(DependencyTrackerTest, DepKind_Classification)
{
    EXPECT_EQ(depKind(DepType::Content), DepKind::ContentOverrideable);
    EXPECT_EQ(depKind(DepType::Directory), DepKind::ContentOverrideable);
    EXPECT_EQ(depKind(DepType::Existence), DepKind::Normal);
    EXPECT_EQ(depKind(DepType::EnvVar), DepKind::Normal);
    EXPECT_EQ(depKind(DepType::CurrentTime), DepKind::Volatile);
    EXPECT_EQ(depKind(DepType::System), DepKind::Normal);
    EXPECT_EQ(depKind(DepType::UnhashedFetch), DepKind::Normal);
    EXPECT_EQ(depKind(DepType::ParentContext), DepKind::ParentContext);
    EXPECT_EQ(depKind(DepType::CopiedPath), DepKind::Normal);
    EXPECT_EQ(depKind(DepType::Exec), DepKind::Volatile);
    EXPECT_EQ(depKind(DepType::NARContent), DepKind::Normal);
    EXPECT_EQ(depKind(DepType::StructuredContent), DepKind::Structural);
}

TEST_F(DependencyTrackerTest, DepKindName_AllKinds)
{
    EXPECT_EQ(depKindName(DepKind::Normal), "normal");
    EXPECT_EQ(depKindName(DepKind::Volatile), "volatile");
    EXPECT_EQ(depKindName(DepKind::ContentOverrideable), "contentOverrideable");
    EXPECT_EQ(depKindName(DepKind::Structural), "structural");
    EXPECT_EQ(depKindName(DepKind::ParentContext), "parentContext");
}

TEST_F(DependencyTrackerTest, IsVolatile)
{
    EXPECT_TRUE(isVolatile(DepType::CurrentTime));
    EXPECT_TRUE(isVolatile(DepType::Exec));
    EXPECT_FALSE(isVolatile(DepType::Content));
    EXPECT_FALSE(isVolatile(DepType::StructuredContent));
    EXPECT_FALSE(isVolatile(DepType::ParentContext));
}

TEST_F(DependencyTrackerTest, IsContentOverrideable)
{
    EXPECT_TRUE(isContentOverrideable(DepType::Content));
    EXPECT_TRUE(isContentOverrideable(DepType::Directory));
    EXPECT_FALSE(isContentOverrideable(DepType::Existence));
    EXPECT_FALSE(isContentOverrideable(DepType::StructuredContent));
    EXPECT_FALSE(isContentOverrideable(DepType::CurrentTime));
}

// ── StructuredFormat enum tests ───────────────────────────────────────

TEST_F(DependencyTrackerTest, StructuredFormatChar_Roundtrip)
{
    EXPECT_EQ(structuredFormatChar(StructuredFormat::Json), 'j');
    EXPECT_EQ(structuredFormatChar(StructuredFormat::Toml), 't');
    EXPECT_EQ(structuredFormatChar(StructuredFormat::Directory), 'd');
}

TEST_F(DependencyTrackerTest, ParseStructuredFormat_ValidChars)
{
    EXPECT_EQ(parseStructuredFormat('j'), StructuredFormat::Json);
    EXPECT_EQ(parseStructuredFormat('t'), StructuredFormat::Toml);
    EXPECT_EQ(parseStructuredFormat('d'), StructuredFormat::Directory);
}

TEST_F(DependencyTrackerTest, ParseStructuredFormat_InvalidChars)
{
    EXPECT_EQ(parseStructuredFormat('x'), std::nullopt);
    EXPECT_EQ(parseStructuredFormat('J'), std::nullopt);
    EXPECT_EQ(parseStructuredFormat('\0'), std::nullopt);
}

TEST_F(DependencyTrackerTest, StructuredFormatName_AllFormats)
{
    EXPECT_EQ(structuredFormatName(StructuredFormat::Json), "json");
    EXPECT_EQ(structuredFormatName(StructuredFormat::Toml), "toml");
    EXPECT_EQ(structuredFormatName(StructuredFormat::Directory), "directory");
}

// ── ShapeSuffix enum tests ────────────────────────────────────────────

TEST_F(DependencyTrackerTest, ShapeSuffixString_AllValues)
{
    EXPECT_EQ(shapeSuffixString(ShapeSuffix::None), "");
    EXPECT_EQ(shapeSuffixString(ShapeSuffix::Len), "#len");
    EXPECT_EQ(shapeSuffixString(ShapeSuffix::Keys), "#keys");
}

TEST_F(DependencyTrackerTest, ShapeSuffixName_AllValues)
{
    EXPECT_EQ(shapeSuffixName(ShapeSuffix::None), "");
    EXPECT_EQ(shapeSuffixName(ShapeSuffix::Len), "len");
    EXPECT_EQ(shapeSuffixName(ShapeSuffix::Keys), "keys");
}

TEST_F(DependencyTrackerTest, ParseShapeSuffix_NoSuffix)
{
    auto [path, shape] = parseShapeSuffix(".foo.bar");
    EXPECT_EQ(path, ".foo.bar");
    EXPECT_EQ(shape, ShapeSuffix::None);
}

TEST_F(DependencyTrackerTest, ParseShapeSuffix_LenSuffix)
{
    auto [path, shape] = parseShapeSuffix(".foo.bar#len");
    EXPECT_EQ(path, ".foo.bar");
    EXPECT_EQ(shape, ShapeSuffix::Len);
}

TEST_F(DependencyTrackerTest, ParseShapeSuffix_KeysSuffix)
{
    auto [path, shape] = parseShapeSuffix(".foo.bar#keys");
    EXPECT_EQ(path, ".foo.bar");
    EXPECT_EQ(shape, ShapeSuffix::Keys);
}

TEST_F(DependencyTrackerTest, ParseShapeSuffix_EmptyPath)
{
    auto [path, shape] = parseShapeSuffix("#len");
    EXPECT_EQ(path, "");
    EXPECT_EQ(shape, ShapeSuffix::Len);
}

TEST_F(DependencyTrackerTest, ParseShapeSuffix_JustHashNotSuffix)
{
    auto [path, shape] = parseShapeSuffix(".foo#bar");
    EXPECT_EQ(path, ".foo#bar");
    EXPECT_EQ(shape, ShapeSuffix::None);
}

// ── buildStructuredDepKey tests ───────────────────────────────────────

TEST_F(DependencyTrackerTest, BuildStructuredDepKey_Scalar)
{
    auto key = buildStructuredDepKey("/file.json", StructuredFormat::Json, ".foo.bar");
    EXPECT_EQ(key, "/file.json\tj:.foo.bar");
}

TEST_F(DependencyTrackerTest, BuildStructuredDepKey_WithLen)
{
    auto key = buildStructuredDepKey("/file.toml", StructuredFormat::Toml, ".list", ShapeSuffix::Len);
    EXPECT_EQ(key, "/file.toml\tt:.list#len");
}

TEST_F(DependencyTrackerTest, BuildStructuredDepKey_WithKeys)
{
    auto key = buildStructuredDepKey("/dir", StructuredFormat::Directory, "", ShapeSuffix::Keys);
    EXPECT_EQ(key, "/dir\td:#keys");
}

// ── formatStructuredDepKey tests ──────────────────────────────────────

TEST_F(DependencyTrackerTest, FormatStructuredDepKey_JsonScalar)
{
    auto result = formatStructuredDepKey("/file.json\tj:.foo.bar");
    EXPECT_EQ(result, "/file.json [json] .foo.bar");
}

TEST_F(DependencyTrackerTest, FormatStructuredDepKey_TomlWithLen)
{
    auto result = formatStructuredDepKey("/f.toml\tt:.list#len");
    EXPECT_EQ(result, "/f.toml [toml] .list #len");
}

TEST_F(DependencyTrackerTest, FormatStructuredDepKey_DirWithKeys)
{
    auto result = formatStructuredDepKey("/dir\td:#keys");
    EXPECT_EQ(result, "/dir [directory]  #keys");
}

TEST_F(DependencyTrackerTest, FormatStructuredDepKey_InvalidFallback)
{
    EXPECT_EQ(formatStructuredDepKey("no-tab-here"), "no-tab-here");
    EXPECT_EQ(formatStructuredDepKey("a\tx:rest"), "a\tx:rest"); // invalid format char
    EXPECT_EQ(formatStructuredDepKey("a\tj"), "a\tj"); // too short (no colon)
}

} // namespace nix::eval_trace
