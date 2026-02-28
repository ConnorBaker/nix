#pragma once
/// @file
/// Shared helpers for hashing interned dep vectors. Used by record(),
/// verifyTrace(), and recovery() — all in trace-store.cc.

#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/hash.hh"

#include <algorithm>
#include <functional>
#include <string_view>
#include <vector>

namespace nix::eval_trace {

using StringLookup = std::function<std::string_view(StringId)>;

/// Sort interned deps by (type, sourceId, keyId) — integer comparison.
/// Dedup on the same triple (different hash values are collapsed).
inline void sortAndDedupInterned(std::vector<TraceStore::InternedDep> & deps)
{
    std::sort(deps.begin(), deps.end(),
        [](const TraceStore::InternedDep & a, const TraceStore::InternedDep & b) {
            if (auto cmp = a.type <=> b.type; cmp != 0) return cmp < 0;
            if (a.sourceId != b.sourceId) return a.sourceId < b.sourceId;
            return a.keyId < b.keyId;
        });
    deps.erase(std::unique(deps.begin(), deps.end(),
        [](const TraceStore::InternedDep & a, const TraceStore::InternedDep & b) {
            return a.type == b.type && a.sourceId == b.sourceId && a.keyId == b.keyId;
        }), deps.end());
}

inline void feedInternedDepToSink(
    HashSink & sink,
    const TraceStore::InternedDep & dep,
    bool includeHash,
    const StringLookup & lookupString)
{
    auto typeStr = std::to_string(static_cast<int>(dep.type));
    sink(std::string_view("T", 1));
    sink(typeStr);
    sink(std::string_view("S", 1));
    sink(lookupString(dep.sourceId));
    sink(std::string_view("K", 1));
    sink(lookupString(dep.keyId));
    if (includeHash) {
        sink(std::string_view("H", 1));
        hashDepValue(sink, dep.hash);
    }
}

/// BLAKE3 hash of sorted interned deps INCLUDING hash values.
inline Hash computeTraceHashFromInterned(
    const std::vector<TraceStore::InternedDep> & sorted,
    const StringLookup & lookupString)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sorted)
        feedInternedDepToSink(sink, dep, true, lookupString);
    return sink.finish().hash;
}

/// BLAKE3 hash of sorted interned deps EXCLUDING hash values (structure only).
inline Hash computeStructHashFromInterned(
    const std::vector<TraceStore::InternedDep> & sorted,
    const StringLookup & lookupString)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sorted)
        feedInternedDepToSink(sink, dep, false, lookupString);
    return sink.finish().hash;
}

} // namespace nix::eval_trace
