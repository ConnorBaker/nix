#pragma once
///@file

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"

#include <string>
#include <map>

namespace nix {

/**
 * The rendered document as a string, with the provenance of what it renders
 * accumulated in `context`. Command outputs copy its mounted inputs before delivery;
 * values retain the context for their consumers.
 * The serialiser is pure: it hands out no stream.
 */
std::string renderValueAsXML(
    EvalState & state, bool strict, bool location, Value & v, NixStringContext & context, const PosIdx pos);

} // namespace nix
