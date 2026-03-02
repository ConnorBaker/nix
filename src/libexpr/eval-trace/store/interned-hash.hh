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

/// Callback to feed a dep key into a HashSink. For most dep types, this
/// resolves the StringId and feeds the string. For ParentContext, this
/// feeds the AttrPathId via AttrVocabStore::hashPath() for deterministic
/// trie-based hashing (independent of string interning order).
using KeyFeeder = std::function<void(HashSink & sink, DepType type, StringId keyId)>;

/// Sort interned deps by (type, sourceId, keyId) — integer comparison.
/// Dedup on the same triple (different hash values are collapsed).
inline void sortAndDedupInterned(std::vector<TraceStore::InternedDep> & deps)
{
    std::sort(deps.begin(), deps.end(),
        [](const TraceStore::InternedDep & a, const TraceStore::InternedDep & b) {
            if (auto cmp = a.key.type <=> b.key.type; cmp != 0) return cmp < 0;
            if (a.key.sourceId != b.key.sourceId) return a.key.sourceId < b.key.sourceId;
            return a.key.keyId < b.key.keyId;
        });
    deps.erase(std::unique(deps.begin(), deps.end(),
        [](const TraceStore::InternedDep & a, const TraceStore::InternedDep & b) {
            return a.key.type == b.key.type && a.key.sourceId == b.key.sourceId && a.key.keyId == b.key.keyId;
        }), deps.end());
}

inline void feedInternedDepToSink(
    HashSink & sink,
    const TraceStore::InternedDep & dep,
    bool includeHash,
    const KeyFeeder & feedKey)
{
    auto typeStr = std::to_string(static_cast<int>(dep.key.type));
    sink(std::string_view("T", 1));
    sink(typeStr);
    sink(std::string_view("S", 1));
    // Source is always a string (even for ParentContext it's empty/"").
    // Feed it via the key feeder with a non-ParentContext type to ensure
    // string resolution. Actually, for source we always resolve as string:
    feedKey(sink, DepType::Content, dep.key.sourceId);  // always string-resolve source
    sink(std::string_view("K", 1));
    feedKey(sink, dep.key.type, dep.key.keyId);
    if (includeHash) {
        sink(std::string_view("H", 1));
        hashDepValue(sink, dep.hash);
    }
}

/// BLAKE3 hash of sorted interned deps INCLUDING hash values.
inline Hash computeTraceHashFromInterned(
    const std::vector<TraceStore::InternedDep> & sorted,
    const KeyFeeder & feedKey)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sorted)
        feedInternedDepToSink(sink, dep, true, feedKey);
    return sink.finish().hash;
}

/// BLAKE3 hash of sorted interned deps EXCLUDING hash values (structure only).
inline Hash computeStructHashFromInterned(
    const std::vector<TraceStore::InternedDep> & sorted,
    const KeyFeeder & feedKey)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & dep : sorted)
        feedInternedDepToSink(sink, dep, false, feedKey);
    return sink.finish().hash;
}

} // namespace nix::eval_trace
