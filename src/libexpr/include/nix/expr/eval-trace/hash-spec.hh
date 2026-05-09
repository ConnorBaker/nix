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

} // namespace nix::eval_trace

namespace nix {

template<>
eval_trace::EvalTraceHashAlgorithm
BaseSetting<eval_trace::EvalTraceHashAlgorithm>::parse(const std::string & str) const;

template<>
std::string BaseSetting<eval_trace::EvalTraceHashAlgorithm>::to_string() const;

} // namespace nix
