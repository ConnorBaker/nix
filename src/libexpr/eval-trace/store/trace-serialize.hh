#pragma once

#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/store/path.hh"
#include "nix/store/sqlite.hh"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nix { struct StoreDirConfig; }

namespace nix::eval_trace {

using PersistedHashBlob = Tagged<struct PersistedHashBlobTag_, std::string>;
using EncodedStorePathBlob = Tagged<struct EncodedStorePathBlobTag_, std::string>;

/// Core blob binding helpers: two overloads cover every persisted-blob
/// wire format. Production call sites pass `encodeX(x).value` directly.
void bindBlob(SQLiteStmt::Use & use, std::string_view blob);
void bindBlob(SQLiteStmt::Use & use, const std::vector<uint8_t> & blob);

void bindRuntimeRootStorePath(
    SQLiteStmt::Use & use,
    const StoreDirConfig & store,
    const RuntimeRootStorePath & storePath);

/// Bind a Tagged<Tag, EvalTraceHash> as a BLOB parameter in a SQLite statement.
template<typename Tag>
void bindTaggedEvalTraceHash(SQLiteStmt::Use & use, const Tagged<Tag, EvalTraceHash> & h)
{
    use(h.value.data(), h.value.size());
}

/// Bind a raw EvalTraceHash as a BLOB parameter in a SQLite statement.
inline void bindEvalTraceHash(SQLiteStmt::Use & use, const EvalTraceHash & h)
{
    use(h.data(), h.size());
}

PersistedHashBlob encodePersistedHashBlob(const Hash & hash);
Hash decodePersistedHashBlob(std::string_view blob);

DepSource decodeRuntimeRootSourceBlob(std::string_view blob);
RuntimeFetchIdentityDepKey decodeRuntimeRootFetchIdentityBlob(std::string_view blob);
RuntimeRootNarHash decodeRuntimeRootNarHashBlob(std::string_view blob);
RuntimeRootStorePath decodeRuntimeRootStorePathBlob(const StoreDirConfig & store, std::string_view blob);

EncodedStorePathBlob encodeStorePathBlob(const StoreDirConfig & store, const StorePath & storePath);
StorePath decodeStorePathBlob(const StoreDirConfig & store, std::string_view blob);

ResultHash computeResultHash(
    ResultKind type,
    uint32_t encodingVersion,
    std::string_view payload,
    std::string_view auxContext);

} // namespace nix::eval_trace
