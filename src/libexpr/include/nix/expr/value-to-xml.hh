#pragma once
///@file

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"

#include <string>
#include <map>

namespace nix {

/**
 * @param unforce with `strict`, unforce each attribute and list
 * element once it is printed. Only for callers that are done with `v`
 * afterwards (command output).
 */
void printValueAsXML(
    EvalState & state,
    bool strict,
    bool location,
    Value & v,
    std::ostream & out,
    NixStringContext & context,
    const PosIdx pos,
    bool unforce = false);

} // namespace nix
