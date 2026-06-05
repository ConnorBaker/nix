#pragma once
///@file

#include "nix/util/configuration.hh"
#include "nix/util/hash.hh"

#include <cstddef>
#include <string>
#include <string_view>

namespace nix::eval_trace {

enum class EvalTraceHashAlgorithm {
    Blake3,
    Sha256,
};

constexpr size_t kEvalTraceDigestSize = 32;

std::string_view evalTraceHashAlgorithmName(EvalTraceHashAlgorithm algorithm);
std::string_view evalTraceHashAlgorithmSlug(EvalTraceHashAlgorithm algorithm);
HashAlgorithm toHashAlgorithm(EvalTraceHashAlgorithm algorithm);
char evalTraceHashAlgorithmTag(EvalTraceHashAlgorithm algorithm);
EvalTraceHashAlgorithm parseEvalTraceHashAlgorithmTag(char tag);

EvalTraceHashAlgorithm getEvalTraceHashAlgorithm();
void setEvalTraceHashAlgorithm(EvalTraceHashAlgorithm algorithm);

/// Process-global eval-trace runtime config (set once at eval startup from
/// EvalSettings, read on the record path). Mirrors the hash-algorithm global
/// above. `eval-trace-defer-flush` — batch per-record SQLite flushes during cold
/// recording (Layer 2a). Default true. See the setting doc in eval-settings.hh.
bool getEvalTraceDeferFlush();
void setEvalTraceDeferFlush(bool deferFlush);

} // namespace nix::eval_trace

namespace nix {

template<>
eval_trace::EvalTraceHashAlgorithm
BaseSetting<eval_trace::EvalTraceHashAlgorithm>::parse(const std::string & str) const;

template<>
std::string BaseSetting<eval_trace::EvalTraceHashAlgorithm>::to_string() const;

} // namespace nix
