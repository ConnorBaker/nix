#pragma once
///@file

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"

#include <string>
#include <map>

namespace nix {

/**
 * The rendered document as a string, with the provenance of what it renders
 * accumulated in `context`, for a sink to `emit` or a value to be made from.
 * The serialiser is pure: it hands out no stream.
 */
std::string renderValueAsXML(
    EvalState & state, bool strict, bool location, Value & v, NixStringContext & context, const PosIdx pos);

} // namespace nix
