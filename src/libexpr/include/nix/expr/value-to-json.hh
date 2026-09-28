#pragma once
///@file

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"

#include <string>
#include <map>
#include <nlohmann/json_fwd.hpp>

namespace nix {

nlohmann::json printValueAsJSON(
    EvalState & state, bool strict, Value & v, const PosIdx pos, NixStringContext & context, bool copyToStore = true);

/**
 * The rendered document as a string, with the provenance of what it renders
 * accumulated in `context`. Command outputs copy its mounted inputs before delivery;
 * values retain the context for their consumers.
 * The serialiser is pure: it hands out no stream.
 */
std::string renderValueAsJSON(
    EvalState & state, bool strict, Value & v, const PosIdx pos, NixStringContext & context, bool copyToStore = true);

MakeError(JSONSerializationError, Error);

} // namespace nix
