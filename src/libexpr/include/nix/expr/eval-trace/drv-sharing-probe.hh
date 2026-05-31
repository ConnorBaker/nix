#pragma once
///@file
/// MEASUREMENT-ONLY probe for RFC §8 step 0
/// (`plans/derivation-producer-partition-rfc.md`): how many DISTINCT attr-path
/// consumer traces force each producer drvPath? Answers the master go/no-go
/// (§7.7b) — if the sharing ratio is ~1, the producer-partition direction is dead.
///
/// Entirely env-gated on `NIX_MEASURE_DRV_SHARING=1`: when unset, every entry
/// point is a single bool test and returns immediately — zero behavioural and
/// near-zero perf effect. NOT wired to any cache decision; pure diagnostics.
///
/// Faithfulness (validated pre-build, RFC pass #18): the "current consumer
/// pathId" is the attr-path `TracedExpr` whose `evaluateResolvedTarget` opened
/// the active scope. On the mapAttrs-over-python3Packages workload each package
/// is a distinct attr-path consumer trace, so a shared infra derivation
/// (stdenv) forced under many package scopes registers many distinct consumer
/// pathIds — exactly the infra-sharing signal §7.7b needs. closures.gnome is the
/// WRONG workload (one coarse string-leaf scope — shape artifact); measure on
/// python3Packages.

#include <cstdint>
#include <string>

namespace nix::eval_trace::drv_sharing_probe {

/// True iff `NIX_MEASURE_DRV_SHARING=1`. Cached. All other entry points no-op
/// when false.
bool enabled();

/// RAII guard set around a consumer `TracedExpr`'s evaluation
/// (`evaluateResolvedTarget`). Stores `pathId` as the current consumer identity
/// on a thread-local stack; restores the previous on scope exit. No-op when
/// `!enabled()`.
struct ConsumerScope
{
    bool active;
    explicit ConsumerScope(uint32_t pathId);
    ~ConsumerScope();
    ConsumerScope(const ConsumerScope &) = delete;
    ConsumerScope & operator=(const ConsumerScope &) = delete;
};

/// Record that `drvPath` was produced while the innermost consumer scope was
/// `current`. Accumulates `drvPath -> set<consumer pathId>` in process-global
/// state. No-op when `!enabled()` or when no consumer scope is active.
void recordProducer(std::string_view drvPath);

/// Dump the distribution (consumers-per-drvPath histogram + summary) to the
/// path in `NIX_MEASURE_DRV_SHARING_PATH` (default stderr). Called from
/// `EvalState::printStatistics`. No-op when `!enabled()`.
void dump();

} // namespace nix::eval_trace::drv_sharing_probe
