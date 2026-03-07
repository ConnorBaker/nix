#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-path.hh"
#include "nix/util/archive.hh"
#include "nix/fetchers/git-utils.hh"

namespace nix {

// ═══════════════════════════════════════════════════════════════════════
// Dep hash functions (BLAKE3 oracle hashing)
// ═══════════════════════════════════════════════════════════════════════

Blake3Hash depHash(std::string_view data)
{
    return Blake3Hash::fromHash(hashString(HashAlgorithm::BLAKE3, data));
}

Blake3Hash depHashPath(const SourcePath & path)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    path.dumpPath(sink);
    return Blake3Hash::fromHash(sink.finish().hash);
}

Blake3Hash depHashDirListing(const SourceAccessor::DirEntries & entries)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & [name, type] : entries) {
        sink(name);
        sink(":");
        int typeInt = type ? static_cast<int>(*type) : -1;
        auto typeStr = std::to_string(typeInt);
        sink(typeStr);
        sink(";");
    }
    return Blake3Hash::fromHash(sink.finish().hash);
}

// ═══════════════════════════════════════════════════════════════════════
// computeGitIdentityHash — git repo fingerprint for GitIdentity dep
// ═══════════════════════════════════════════════════════════════════════

std::optional<Blake3Hash> computeGitIdentityHash(const std::filesystem::path & repoRoot)
{
    auto wd = GitRepo::getCachedWorkdirInfo(repoRoot);
    if (!wd.headRev)
        return std::nullopt;

    // Build identity string: HEAD rev + dirty file fingerprint
    // (same SHA512 HashSink pattern as git fetcher's getFingerprint)
    std::string identity = "git-rev=" + wd.headRev->gitRev();
    if (wd.isDirty) {
        HashSink hashSink{HashAlgorithm::SHA512};
        for (auto & file : wd.dirtyFiles) {
            writeString("modified:", hashSink);
            writeString(file.abs(), hashSink);
            dumpPath(repoRoot / file.rel(), hashSink);
        }
        for (auto & file : wd.deletedFiles) {
            writeString("deleted:", hashSink);
            writeString(file.abs(), hashSink);
        }
        identity += ";git-dirty=" + hashSink.finish().hash.to_string(HashFormat::Base16, false);
    }
    return depHash(identity);
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
