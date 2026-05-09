#pragma once

#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"

#include <iterator>
#include <optional>
#include <vector>

namespace nix::eval_trace {

struct PendingPrecomputedKey {
    uint32_t originOffset;
    PrecomputedKeysInfo info;
};

class MaterializationScope
{
    struct StagedValueIdentity {
        Value * value = nullptr;
        ValueIdentityStamp stamp;
    };

    struct StagedContainerProvenance {
        Value * value = nullptr;
        DepSourceId sourceId;
        FilePathId filePathId;
        DataPathId dataPathId;
        StructuredFormat format;
    };

    EvalState & state;
    TraceBackend * backend = nullptr;
    AttrPathId pathId;
    std::optional<TraceAccess> access;
    std::vector<StagedValueIdentity> valueIdentities;
    std::vector<StagedContainerProvenance> containerProvenances;
    std::vector<PendingPrecomputedKey> precomputedKeys;

public:
    MaterializationScope(
        EvalState & state,
        TraceBackend * backend,
        AttrPathId pathId,
        std::optional<TraceAccess> access)
        : state(state)
        , backend(backend)
        , pathId(pathId)
        , access(std::move(access))
    {
    }

    MaterializationScope(const MaterializationScope &) = delete;
    MaterializationScope & operator=(const MaterializationScope &) = delete;

    void stageValueIdentity(Value & value, ValueIdentityStamp stamp)
    {
        valueIdentities.push_back(StagedValueIdentity{
            .value = &value,
            .stamp = stamp,
        });
    }

    void stageContainerProvenance(
        Value & value,
        DepSourceId sourceId,
        FilePathId filePathId,
        DataPathId dataPathId,
        StructuredFormat format)
    {
        containerProvenances.push_back(StagedContainerProvenance{
            .value = &value,
            .sourceId = sourceId,
            .filePathId = filePathId,
            .dataPathId = dataPathId,
            .format = format,
        });
    }

    void stagePrecomputedKeys(std::vector<PendingPrecomputedKey> batch)
    {
        if (batch.empty())
            return;
        precomputedKeys.insert(
            precomputedKeys.end(),
            std::make_move_iterator(batch.begin()),
            std::make_move_iterator(batch.end()));
    }

    void commit();
};

} // namespace nix::eval_trace
