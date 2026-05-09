#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-path.hh"
#include "nix/util/archive.hh"
#include "nix/fetchers/git-utils.hh"

#include <algorithm>
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

std::optional<CurrentGitIdentityHash> computeGitIdentityHash(const std::filesystem::path & repoRoot)
{
    auto wd = GitRepo::openRepo(repoRoot, {})->getWorkdirInfo();
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
