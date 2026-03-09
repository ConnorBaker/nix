#pragma once

#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/store/sqlite.hh"

#include <cstdint>
#include <string_view>
#include <vector>

namespace nix::eval_trace {

/// Bind a TypedHash (32-byte BLAKE3) as a BLOB parameter in a SQLite statement.
template<typename Tag>
void bindTypedHash(SQLiteStmt::Use & use, const TypedHash<Tag> & h)
{
    use(h.raw().data(), h.raw().size());
}

/// Bind a raw Blake3Hash as a BLOB parameter in a SQLite statement.
inline void bindBlake3Hash(SQLiteStmt::Use & use, const Blake3Hash & h)
{
    use(h.data(), h.size());
}

void bindBlobVec(SQLiteStmt::Use & use, const std::vector<uint8_t> & blob);

ResultHash computeResultHash(ResultKind type, std::string_view value, std::string_view context);

} // namespace nix::eval_trace
