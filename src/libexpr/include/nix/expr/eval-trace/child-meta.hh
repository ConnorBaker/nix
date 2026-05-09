#pragma once
///@file

#include "nix/expr/eval-trace/ids.hh"
#include "nix/expr/eval-trace/semantic-objects.hh"
#include "nix/expr/symbol-table.hh"

#include <cstdint>
#include <optional>

namespace nix::eval_trace {

struct TracedContainerMeta {
    std::optional<StructuredObject> producerOrigin;
    std::optional<ValueIdentityStamp> valueIdentityStamp;

    bool operator==(const TracedContainerMeta &) const = default;
};

struct CachedAttrEntry {
    Symbol name;
    std::optional<StructuredObject> producerOrigin;
    uint32_t aliasOf = invalidSiblingIndex;
};

struct CachedListEntry {
    uint32_t aliasOf = invalidSiblingIndex;
};

} // namespace nix::eval_trace
