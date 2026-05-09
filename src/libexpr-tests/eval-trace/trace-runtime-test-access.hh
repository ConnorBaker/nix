#pragma once
///@file
/// Test-only friend access to TraceRuntime's private `_ForTest` methods.
/// Production callers have no path to these names; the `ForTest` suffix
/// used to be part of the public API but was moved here per P20.

#include "nix/expr/eval-trace/context.hh"

namespace nix::eval_trace::test {

struct TraceRuntimeTestAccess {
    static std::vector<Dep> & epochLog(TraceRuntime & r) { return r.epochLog_ForTest(); }
    static std::optional<DepRange> lookupReplayRange(const TraceRuntime & r, const Value & v)
    { return r.lookupReplayRange_ForTest(v); }
    static bool hasReplayEntries(const TraceRuntime & r) { return r.hasReplayEntries_ForTest(); }
    static void clearReplayEntries(TraceRuntime & r) { r.clearReplayEntries_ForTest(); }
    static void registerValueIdentity(
        TraceRuntime & r, const Value * key, AttrPathId pathId = AttrPathId())
    { r.registerValueIdentity_ForTest(key, pathId); }
    static bool hasValueIdentity(const TraceRuntime & r, const Value * key)
    { return r.hasValueIdentity_ForTest(key); }
    static bool hasBindingsValueIdentity(const TraceRuntime & r, const Bindings * key)
    { return r.hasBindingsValueIdentity_ForTest(key); }
    static void reset(TraceRuntime & r) { r.reset_ForTest(); }
    static void recordThunkDeps(TraceRuntime & r, const Value & v, uint32_t epochStart)
    { r.recordThunkDeps_ForTest(v, epochStart); }
    static void replayMemoizedDeps(TraceRuntime & r, const Value & v)
    { r.replayMemoizedDeps_ForTest(v); }
};

} // namespace nix::eval_trace::test
