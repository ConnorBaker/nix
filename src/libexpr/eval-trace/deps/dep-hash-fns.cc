#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-path.hh"
#include "nix/util/archive.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/users.hh"
#include "nix/util/file-system.hh"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>

namespace nix {

// ═══════════════════════════════════════════════════════════════════════
// Dep hash functions (eval-trace oracle hashing)
// ═══════════════════════════════════════════════════════════════════════

DepHash depHash(std::string_view data)
{
    return DepHash{
        EvalTraceHash::fromHash(hashString(eval_trace::toHashAlgorithm(
            eval_trace::getEvalTraceHashAlgorithm()), data))};
}

DepHash depHashPath(const SourcePath & path)
{
    HashSink sink(eval_trace::toHashAlgorithm(eval_trace::getEvalTraceHashAlgorithm()));
    path.dumpPath(sink);
    return DepHash{EvalTraceHash::fromHash(sink.finish().hash)};
}

DepHash depHashDirListing(const SourceAccessor::DirEntries & entries)
{
    auto builder = eval_trace::makeDomainBuilder<eval_trace::hash_domain::DepHashDirListing>();
    std::vector<std::pair<std::string, int64_t>> sortedEntries;
    sortedEntries.reserve(entries.size());
    for (const auto & [name, type] : entries) {
        sortedEntries.emplace_back(
            name,
            type ? static_cast<int64_t>(*type) : int64_t{-1});
    }
    std::sort(sortedEntries.begin(), sortedEntries.end());

    builder.field("entry-count", static_cast<uint64_t>(sortedEntries.size()));
    for (const auto & [name, type] : sortedEntries) {
        builder.field("entry-name", name);
        builder.field("entry-type", type);
    }
    return DepHash{builder.finish()};
}

// ═══════════════════════════════════════════════════════════════════════
// computeGitIdentityHash — git repo fingerprint for GitIdentity dep
// ═══════════════════════════════════════════════════════════════════════

// Build the GitIdentity hash from a WorkdirInfo. Extracted from computeGitIdentityHash
// so the G1 #2 cache fast-path can build the IDENTICAL clean identity from a synthetic
// clean WorkdirInfo (head-rev + 0 modified + 0 deleted + worktree-dirty:false).
static std::optional<CurrentGitIdentityHash> buildGitIdentityFromWorkdirInfo(
    const std::filesystem::path & repoRoot, const GitRepo::WorkdirInfo & wd)
{
    if (!wd.headRev)
        return std::nullopt;

    auto builder = eval_trace::makeDomainBuilder<eval_trace::hash_domain::GitIdentity>();
    builder.field("head-rev", wd.headRev->gitRev());

    struct ModifiedEntry {
        std::string absolutePath;
        EvalTraceHash contentHash;
    };
    struct DeletedEntry {
        std::string absolutePath;
    };

    std::vector<ModifiedEntry> modifiedEntries;
    modifiedEntries.reserve(wd.dirtyFiles.size());
    for (const auto & file : wd.dirtyFiles) {
        HashSink sink{eval_trace::toHashAlgorithm(eval_trace::getEvalTraceHashAlgorithm())};
        dumpPath(repoRoot / file.rel(), sink);
        modifiedEntries.push_back(ModifiedEntry{
            .absolutePath = file.abs(),
            .contentHash = EvalTraceHash::fromHash(sink.finish().hash),
        });
    }
    std::sort(modifiedEntries.begin(), modifiedEntries.end(), [](const auto & lhs, const auto & rhs) {
        return lhs.absolutePath < rhs.absolutePath;
    });

    std::vector<DeletedEntry> deletedEntries;
    deletedEntries.reserve(wd.deletedFiles.size());
    for (const auto & file : wd.deletedFiles) {
        deletedEntries.push_back(DeletedEntry{
            .absolutePath = file.abs(),
        });
    }
    std::sort(deletedEntries.begin(), deletedEntries.end(), [](const auto & lhs, const auto & rhs) {
        return lhs.absolutePath < rhs.absolutePath;
    });

    builder.field("modified-count", static_cast<uint64_t>(modifiedEntries.size()));
    for (const auto & entry : modifiedEntries) {
        builder.field("modified-path", entry.absolutePath);
        builder.field("modified-content-hash", entry.contentHash);
    }

    builder.field("deleted-count", static_cast<uint64_t>(deletedEntries.size()));
    for (const auto & entry : deletedEntries)
        builder.field("deleted-path", entry.absolutePath);

    if (!wd.dirtyFiles.empty() || !wd.deletedFiles.empty()) {
        builder.field("worktree-dirty", true);
    } else {
        builder.field("worktree-dirty", false);
    }

    return CurrentGitIdentityHash{builder.finish()};
}

// G1 PROTOTYPE option #2 (plans/eval-trace-perf-cold-and-hot.md "G1 PROTOTYPE"): a
// cross-process cache of the "worktree is clean" verdict, keyed on a CHEAP token
// (HEAD oid + .git/index mtime+size). On a clean hit we skip the O(worktree) git_status
// scan and build the IDENTICAL clean identity. This is a PRECISION optimisation (skip a
// scan), NOT a soundness mechanism: the soundness FLOOR is the per-file content-based
// FileBytes/DirectoryEntries backstop, which re-reads and re-hashes actual content at
// verify. A staged change (git add/commit) normally bumps the index mtime/size -> token
// miss -> re-scan; a new commit bumps HEAD -> miss; an UNSTAGED edit (index unchanged) ->
// stale "clean" hit -> the content backstop catches any result-dep change at verify; a
// dirtied non-result-dep file is harmless (the result doesn't depend on it). Dirty
// worktrees never cache.
//
// CAVEAT (2026-06-04 — the failure-mode dig): the ".git/index mtime+size" token is an
// mtime token, and mtime on a COARSE-granularity filesystem (e.g. ZFS-on-Linux, ~timer-
// tick ~10ms at HZ=100; vs tmpfs which is nanosecond) does NOT reliably advance between
// two rapid same-size writes (the kernel coarse realtime clock returns the same value
// within a tick). So "a staged change bumps the index mtime -> miss" is not guaranteed
// for two index writes within a tick that leave the same size. This only costs PRECISION
// here because the content FileBytes/DirectoryEntries backstop is the soundness floor — and
// that floor must stay content-accurate: the verify path re-reads and re-hashes actual
// content (no mtime shortcut), so an unstaged same-size edit to a result-dep file is caught
// even when this marker stale-"clean"-hits. (A posix mtime-token FileBytes shortcut, "HOT-1",
// was prototyped and REMOVED precisely because it would have made that floor mtime-based on
// coarse FS. See doc/eval-trace/measurements/property-test-overinvalidation-2026-06-04.md.)
//
// DEFAULT-ON: cached unless NIX_EVAL_TRACE_GIT_NO_CACHE=1 (see computeGitIdentityHash).
// The marker is a 0-byte file under getCacheDir(); its presence == "clean for this token".
static std::string gitCleanToken(const std::filesystem::path & repoRoot, const Hash & headRev)
{
    std::string t = headRev.gitRev();
    std::error_code ec;
    auto idx = repoRoot / ".git" / "index";
    auto mt = std::filesystem::last_write_time(idx, ec);
    if (!ec) {
        std::error_code ec2;
        auto sz = std::filesystem::file_size(idx, ec2);
        t += ":" + std::to_string(mt.time_since_epoch().count()) + ":"
             + (ec2 ? std::string("?") : std::to_string(sz));
    } else
        t += ":noindex";
    return t;
}

static std::filesystem::path gitCleanMarkerPath(
    const std::filesystem::path & repoRoot, const std::string & token)
{
    auto digest = hashString(HashAlgorithm::SHA256, repoRoot.string() + std::string("\0", 1) + token)
                      .to_string(HashFormat::Nix32, false);
    return std::filesystem::path(getCacheDir()) / "eval-trace-git-clean-v1" / digest;
}

// Bound the marker dir: delete markers older than ttlSeconds. Markers accumulate one per
// (repo, HEAD, index) state ever seen (0 bytes each), so this caps growth at ~one TTL window
// of distinct states. Called only on a cache MISS (already paying the O(worktree) scan, so the
// sweep is hidden) AND throttled to ~hourly via a `.last-gc` sentinel so a heavy-CI stream of
// misses (each a fresh commit) does not pay O(markers) per eval. A GC'd-but-still-active marker
// simply causes one re-scan that recreates it. Best-effort; never throws.
static void maybeSweepGitCleanMarkers(const std::filesystem::path & dir)
{
    constexpr int64_t ttlSeconds = 30 * 24 * 3600;   // 30 days
    constexpr int64_t throttleSeconds = 3600;         // sweep at most ~hourly
    using clock = std::filesystem::file_time_type::clock;
    auto now = clock::now();
    auto sentinel = dir / ".last-gc";
    std::error_code ec;
    auto last = std::filesystem::last_write_time(sentinel, ec);
    if (!ec && std::chrono::duration_cast<std::chrono::seconds>(now - last).count() < throttleSeconds)
        return;
    // Touch the sentinel first so concurrent processes don't all sweep at once.
    try { writeFile(sentinel.string(), ""); } catch (...) { return; }
    std::filesystem::directory_iterator it(dir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it->path().filename() == ".last-gc") continue;
        std::error_code ec2;
        auto mt = it->last_write_time(ec2);
        if (ec2) continue;
        if (std::chrono::duration_cast<std::chrono::seconds>(now - mt).count() > ttlSeconds)
            std::filesystem::remove(it->path(), ec2);
    }
}

std::optional<CurrentGitIdentityHash> computeGitIdentityHash(const std::filesystem::path & repoRoot)
{
    // DEFAULT PATH (2026-06-02): the worktree git-identity is cached cross-process. The
    // O(worktree) git_status scan — measured ~0.6s ≈ 64% of the `-f /git` warm hot wall and
    // 100% eval-trace's (perf STAT-SOURCE PROFILE) — runs ONLY on a cache miss (first eval per
    // HEAD/index state, or a dirty worktree). Escape hatch: NIX_EVAL_TRACE_GIT_NO_CACHE=1 forces
    // the always-scan path. SOUNDNESS: identical to always-scan for every reachable case — the
    // broader edit-case pass (plans/eval-trace-perf-cold-and-hot.md "G1 #2 SOUNDNESS PASS")
    // found 0 regressions across all dep kinds. The only behavioral difference is for an UNSTAGED
    // edit between index changes: the always-scan path invalidates the whole session (worktree
    // dirty -> session key changes); the cache reuses the session key and the per-file dep
    // backstop (FileBytes/DirectoryEntries/ExistenceCheck/DerivedStorePath/...) catches the
    // change at verify instead. Staged/committed changes bump the token -> re-scan (identical).
    static const bool noCache = getEnv("NIX_EVAL_TRACE_GIT_NO_CACHE").value_or("") == "1";
    if (!noCache) {
        auto repo = GitRepo::openRepo(repoRoot, {});
        std::optional<Hash> headRev;
        try {
            headRev = repo->resolveRef("HEAD");
        } catch (...) {
        }
        if (headRev) {
            auto marker = gitCleanMarkerPath(repoRoot, gitCleanToken(repoRoot, *headRev));
            if (pathExists(marker.string())) {
                // Cached clean: build the identical clean identity, skip the scan.
                GitRepo::WorkdirInfo cleanWd;
                cleanWd.headRev = *headRev;
                return buildGitIdentityFromWorkdirInfo(repoRoot, cleanWd);
            }
            auto wd = repo->getWorkdirInfo();
            auto id = buildGitIdentityFromWorkdirInfo(repoRoot, wd);
            if (wd.headRev && !wd.isDirty) {
                try {
                    createDirs(marker.parent_path().string());
                    writeFile(marker.string(), "");
                    maybeSweepGitCleanMarkers(marker.parent_path());
                } catch (...) {
                    // best-effort cache write
                }
            }
            return id;
        }
        // HEAD did not resolve: fall through to the default (uncached) path.
    }

    auto wd = GitRepo::openRepo(repoRoot, {})->getWorkdirInfo();
    return buildGitIdentityFromWorkdirInfo(repoRoot, wd);
}

// ═══════════════════════════════════════════════════════════════════════
// dirEntryTypeString
// ═══════════════════════════════════════════════════════════════════════

std::string dirEntryTypeString(std::optional<SourceAccessor::Type> type)
{
    if (!type) return "unknown";
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (*type) {
    case SourceAccessor::tRegular: return "regular";
    case SourceAccessor::tDirectory: return "directory";
    case SourceAccessor::tSymlink: return "symlink";
    default: return "unknown";
    }
#pragma GCC diagnostic pop
}

} // namespace nix
