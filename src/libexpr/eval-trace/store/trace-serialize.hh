#pragma once

#include "nix/expr/eval-trace/result.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/hash.hh"

#include <cstdint>
#include <string_view>
#include <vector>

namespace nix::eval_trace {

void bindRawHash(SQLiteStmt::Use & use, const Hash & h);
Hash readRawHash(const void * data, size_t size);
void bindBlobVec(SQLiteStmt::Use & use, const std::vector<uint8_t> & blob);

Hash computeResultHash(ResultKind type, std::string_view value, std::string_view context);

} // namespace nix::eval_trace
