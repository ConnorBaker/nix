#pragma once
/// @file
/// Typed navigation selectors for traced-expression re-navigation.
/// Private header for eval-trace cache internals.

#include "nix/expr/nixexpr.hh"

#include <variant>
#include <vector>

namespace nix::eval_trace {

struct AttrSelector {
    Symbol name;
};

struct ListSelector {
    size_t index;
};

using ChildSelector = std::variant<AttrSelector, ListSelector>;
using NodeLocator = std::vector<ChildSelector>;

} // namespace nix::eval_trace
