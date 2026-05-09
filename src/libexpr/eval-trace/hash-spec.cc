#include "nix/expr/eval-trace/hash-spec.hh"

#include "nix/util/abstract-setting-to-json.hh"
#include "nix/util/config-impl.hh"
#include "nix/util/error.hh"

#include <atomic>
#include <nlohmann/json.hpp>

namespace nix::eval_trace {

namespace {

std::atomic<EvalTraceHashAlgorithm> activeAlgorithm{EvalTraceHashAlgorithm::Blake3};

}

std::string_view evalTraceHashAlgorithmName(EvalTraceHashAlgorithm algorithm)
{
    switch (algorithm) {
    case EvalTraceHashAlgorithm::Blake3:
        return "blake3";
    case EvalTraceHashAlgorithm::Sha256:
        return "sha256";
    }
    unreachable();
}

std::string_view evalTraceHashAlgorithmSlug(EvalTraceHashAlgorithm algorithm)
{
    return evalTraceHashAlgorithmName(algorithm);
}

HashAlgorithm toHashAlgorithm(EvalTraceHashAlgorithm algorithm)
{
    switch (algorithm) {
    case EvalTraceHashAlgorithm::Blake3:
        return HashAlgorithm::BLAKE3;
    case EvalTraceHashAlgorithm::Sha256:
        return HashAlgorithm::SHA256;
    }
    unreachable();
}

char evalTraceHashAlgorithmTag(EvalTraceHashAlgorithm algorithm)
{
    switch (algorithm) {
    case EvalTraceHashAlgorithm::Blake3:
        return 'b';
    case EvalTraceHashAlgorithm::Sha256:
        return 's';
    }
    unreachable();
}

EvalTraceHashAlgorithm parseEvalTraceHashAlgorithmTag(char tag)
{
    switch (tag) {
    case 'b':
        return EvalTraceHashAlgorithm::Blake3;
    case 's':
        return EvalTraceHashAlgorithm::Sha256;
    default:
        throw Error("invalid eval-trace hash algorithm tag %d",
            static_cast<unsigned char>(tag));
    }
}

EvalTraceHashAlgorithm getEvalTraceHashAlgorithm()
{
    return activeAlgorithm.load(std::memory_order_relaxed);
}

void setEvalTraceHashAlgorithm(EvalTraceHashAlgorithm algorithm)
{
    activeAlgorithm.store(algorithm, std::memory_order_relaxed);
}

} // namespace nix::eval_trace

namespace nix {

template<>
eval_trace::EvalTraceHashAlgorithm
BaseSetting<eval_trace::EvalTraceHashAlgorithm>::parse(const std::string & str) const
{
    if (str == "blake3")
        return eval_trace::EvalTraceHashAlgorithm::Blake3;
    if (str == "sha256")
        return eval_trace::EvalTraceHashAlgorithm::Sha256;
    throw UsageError("option '%s' has invalid value '%s'", name, str);
}

template<>
struct BaseSetting<eval_trace::EvalTraceHashAlgorithm>::trait
{
    static constexpr bool appendable = false;
};

template<>
std::string BaseSetting<eval_trace::EvalTraceHashAlgorithm>::to_string() const
{
    return std::string(eval_trace::evalTraceHashAlgorithmName(value));
}

NLOHMANN_JSON_SERIALIZE_ENUM(
    eval_trace::EvalTraceHashAlgorithm,
    {
        {eval_trace::EvalTraceHashAlgorithm::Blake3, "blake3"},
        {eval_trace::EvalTraceHashAlgorithm::Sha256, "sha256"},
    });

template class BaseSetting<eval_trace::EvalTraceHashAlgorithm>;

} // namespace nix
