#include "helpers.hh"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

namespace nix::eval_trace::test {

static void expectPathObjectEq(
    const std::optional<PathObject> & a,
    const std::optional<PathObject> & b,
    const char * label)
{
    ASSERT_EQ(a.has_value(), b.has_value())
        << label << " origin presence mismatch";
    if (!a)
        return;

    EXPECT_EQ(a->source, b->source) << label << " origin source mismatch";
    EXPECT_EQ(a->rootPath, b->rootPath) << label << " origin rootPath mismatch";
}

static void expectTextObjectEq(
    const std::optional<TextObject> & a,
    const std::optional<TextObject> & b,
    const char * label)
{
    ASSERT_EQ(a.has_value(), b.has_value())
        << label << " readFile provenance presence mismatch";
    if (!a)
        return;

    EXPECT_EQ(a->source, b->source)
        << label << " readFile provenance source mismatch";
    EXPECT_EQ(a->key, b->key)
        << label << " readFile provenance key mismatch";
    EXPECT_EQ(a->contentHash, b->contentHash)
        << label << " readFile provenance content hash mismatch";
}

static void expectProducerOriginEq(
    const std::optional<StructuredObject> & a,
    const std::optional<StructuredObject> & b,
    const char * label,
    size_t index = 0)
{
    ASSERT_EQ(a.has_value(), b.has_value())
        << label << " producerOrigin presence mismatch at " << index;
    if (!a)
        return;

    EXPECT_EQ(a->source, b->source) << label << " depSource mismatch at " << index;
    EXPECT_EQ(a->key, b->key) << label << " depKey mismatch at " << index;
    EXPECT_EQ(a->dataPath, b->dataPath) << label << " dataPath mismatch at " << index;
    EXPECT_EQ(a->format, b->format) << label << " format mismatch at " << index;
}

void assertCachedResultEquals(const CachedResult & a, const CachedResult & b, SymbolTable & symbols)
{
    ASSERT_EQ(a.index(), b.index()) << "CachedResult variant index mismatch";

    std::visit(overloaded{
        [&](const attrs_t & va) {
            auto & vb = std::get<attrs_t>(b);
            ASSERT_EQ(va.entries.size(), vb.entries.size()) << "FullAttrs: different number of children";
            ASSERT_EQ(va.meta.has_value(), vb.meta.has_value())
                << "FullAttrs meta presence mismatch";
            if (va.meta) {
                expectProducerOriginEq(
                    va.meta->producerOrigin,
                    vb.meta->producerOrigin,
                    "FullAttrs meta");
                EXPECT_EQ(va.meta->valueIdentityStamp, vb.meta->valueIdentityStamp)
                    << "FullAttrs meta valueIdentityStamp mismatch";
            }
            for (size_t i = 0; i < va.entries.size(); i++)
                EXPECT_EQ(std::string_view(symbols[va.entries[i].name]),
                           std::string_view(symbols[vb.entries[i].name]))
                    << "FullAttrs: child name mismatch at index " << i;
            for (size_t i = 0; i < va.entries.size(); i++) {
                expectProducerOriginEq(
                    va.entries[i].producerOrigin,
                    vb.entries[i].producerOrigin,
                    "FullAttrs child",
                    i);
                EXPECT_EQ(va.entries[i].aliasOf, vb.entries[i].aliasOf)
                    << "child aliasOf mismatch at " << i;
            }
        },
        [&](const string_t & va) {
            auto & vb = std::get<string_t>(b);
            EXPECT_EQ(va.first, vb.first) << "String: value mismatch";
            EXPECT_EQ(va.second, vb.second) << "String: context mismatch";
            expectPathObjectEq(va.publication.path, vb.publication.path, "String");
            expectTextObjectEq(
                va.publication.text,
                vb.publication.text,
                "String");
        },
        [&](const trivial_t & va) {
            ASSERT_TRUE(std::holds_alternative<trivial_t>(b))
                << "Trivial expected; got kind index " << b.index();
            auto & vb = std::get<trivial_t>(b);
            EXPECT_EQ(va.kind, vb.kind)
                << "Trivial sub-kind mismatch: "
                << trivialKindName(va.kind) << " vs " << trivialKindName(vb.kind);
        },
        [&](const failed_t &) {
            EXPECT_TRUE(std::holds_alternative<failed_t>(b));
        },
        [&](bool va) {
            EXPECT_EQ(va, std::get<bool>(b)) << "Bool mismatch";
        },
        [&](const int_t & va) {
            EXPECT_EQ(va.x.value, std::get<int_t>(b).x.value) << "Int mismatch";
        },
        [&](const path_t & va) {
            EXPECT_EQ(va.path, std::get<path_t>(b).path) << "Path mismatch";
            expectPathObjectEq(
                va.publication.path,
                std::get<path_t>(b).publication.path,
                "Path");
        },
        [&](const float_t & va) {
            EXPECT_DOUBLE_EQ(va.x, std::get<float_t>(b).x) << "Float mismatch";
        },
        [&](const list_t & va) {
            auto & vb = std::get<list_t>(b);
            EXPECT_EQ(va.entries.size(), vb.entries.size()) << "List: size mismatch";
            ASSERT_EQ(va.meta.has_value(), vb.meta.has_value())
                << "List meta presence mismatch";
            if (va.meta) {
                expectProducerOriginEq(
                    va.meta->producerOrigin,
                    vb.meta->producerOrigin,
                    "List meta");
                EXPECT_EQ(va.meta->valueIdentityStamp, vb.meta->valueIdentityStamp)
                    << "List meta valueIdentityStamp mismatch";
            }
            for (size_t i = 0; i < va.entries.size(); i++) {
                EXPECT_EQ(va.entries[i].aliasOf, vb.entries[i].aliasOf)
                    << "List child aliasOf mismatch at " << i;
            }
        },
    }, a);
}

// ── TempGitRepo implementation ──────────────────────────────────────

namespace {

/// Run a shell command inside a given directory. Throws on non-zero exit.
static void runGit(const std::string & cmd, const std::filesystem::path & dir)
{
    auto full = "cd " + dir.string() + " && " + cmd;
    int ret = std::system(full.c_str()); // NOLINT(cert-env33-c)
    if (ret != 0)
        throw std::runtime_error("git command failed: " + cmd);
}

/// Run a shell command in dir and capture stdout. Returns trimmed output.
static std::string captureGit(const std::string & cmd, const std::filesystem::path & dir)
{
    auto full = "cd " + dir.string() + " && " + cmd;
    FILE * pipe = popen(full.c_str(), "r"); // NOLINT(cert-env33-c)
    if (!pipe)
        throw std::runtime_error("popen failed for: " + cmd);
    std::string result;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe))
        result += buf;
    pclose(pipe);
    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

} // anonymous namespace

TempGitRepo::TempGitRepo()
{
    auto base = std::filesystem::canonical(std::filesystem::temp_directory_path()) / "nix-test-eval-trace";
    createDirs(base);

    static std::atomic<int> counter = 0;
    auto tmpl = base / ("git-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
    std::filesystem::create_directory(tmpl);
    repoPath = tmpl;

    runGit("git init -q", repoPath);
    runGit("git config user.email 'test@nix-eval-trace'", repoPath);
    runGit("git config user.name 'Test'", repoPath);

    // Create a .gitignore and make an initial commit so HEAD exists.
    {
        std::ofstream ofs(repoPath / ".gitignore");
        ofs << "# test repo\n";
    }
    runGit("git add .gitignore", repoPath);
    runGit("git commit -q -m 'initial commit'", repoPath);
}

TempGitRepo::~TempGitRepo()
{
    try {
        deletePath(repoPath);
    } catch (...) {
    }
}

void TempGitRepo::addFile(const std::string & name, const std::string & content)
{
    std::ofstream ofs(repoPath / name);
    ofs << content;
}

std::string TempGitRepo::commit(const std::string & message)
{
    runGit("git add -A", repoPath);
    runGit("git commit -q -m '" + message + "'", repoPath);
    return headHash();
}

std::string TempGitRepo::headHash() const
{
    return captureGit("git rev-parse HEAD", repoPath);
}

void TempGitRepo::dirtyModify(const std::string & name, const std::string & newContent)
{
    std::ofstream ofs(repoPath / name, std::ios::trunc);
    ofs << newContent;
}

std::filesystem::path TempGitRepo::filePath(const std::string & name) const
{
    return repoPath / name;
}

} // namespace nix::eval_trace::test
