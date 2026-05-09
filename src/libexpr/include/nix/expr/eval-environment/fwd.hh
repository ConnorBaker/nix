#pragma once

namespace nix {

class EvalEnvironment;
class EvalState;
class Store;
class TraceSessionFactory;
struct EvalSettings;
struct LookupPath;
struct MemorySourceAccessor;
struct MountedSourceAccessor;
struct Value;

namespace fetchers {
struct Input;
struct InputCache;
struct Settings;
}

namespace eval_trace {
class TraceSession;
struct TraceAccess;
}

} // namespace nix
