#pragma once
///@file

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"

#include <string>
#include <map>
#include <nlohmann/json_fwd.hpp>

namespace nix {

/**
 * @param unforce PROTOTYPE: unforce each attribute and list element once it
 * is serialised. Only for callers that are done with `v` afterwards (command
 * output), never for builtins.
 */
nlohmann::json printValueAsJSON(
    EvalState & state,
    bool strict,
    Value & v,
    const PosIdx pos,
    NixStringContext & context,
    bool copyToStore = true,
    bool unforce = false);

void printValueAsJSON(
    EvalState & state,
    bool strict,
    Value & v,
    const PosIdx pos,
    std::ostream & str,
    NixStringContext & context,
    bool copyToStore = true,
    bool unforce = false);

MakeError(JSONSerializationError, Error);

} // namespace nix
