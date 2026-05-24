#pragma once
/**
 * @file
 *
 * Template implementations (as opposed to mere declarations).
 *
 * This file is an example of the "impl.hh" pattern. See the
 * contributing guide.
 */

#include "nix/store/build-result.hh"
#include "nix/store/common-protocol.hh"
#include "nix/store/length-prefixed-protocol-helper.hh"
#include "nix/store/realisation.hh"
#include "nix/util/hash.hh"
#include "nix/util/json-utils.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>

NIX_DEFINE_LENGTH_PREFIX_SERIALISERS(CommonProto)

namespace nix {

struct StoreDirConfig;

/* Cross-protocol serialiser bodies shared between WorkerProto and
   ServeProto. Each helper is parameterised on the protocol class and
   takes its per-protocol version-gate decisions as boolean predicates;
   the per-protocol callers compute those predicates from their own
   version structure (which differ in shape: WorkerProto uses
   number+features, ServeProto uses major+minor only).

   Wire format is byte-for-byte identical to the previous per-protocol
   bodies. */

/**
 * Read the BuildResult body shared between WorkerProto and ServeProto.
 *
 * - @p hasV2Fields gates the (timesBuilt, isNonDeterministic, startTime,
 *   stopTime) tuple. Worker: `>= {1, 29}`. Serve: `>= {2, 3}`.
 * - @p readCpuTiming is a per-protocol callable invoked between the V2
 *   tuple and the builtOutputs block. Worker writes (cpuUser, cpuSystem)
 *   guarded on `>= {1, 37}`; Serve passes a no-op. The hook exists
 *   because `WorkerProto` has a `Serialise<std::optional<chrono>>`
 *   specialisation that `ServeProto` lacks, so a runtime branch in the
 *   shared body would still require the templates to instantiate.
 * - @p hasNewBuiltOutputs gates the post-rework `map<OutputName, UnkeyedRealisation>`
 *   format. Worker: feature `realisation-with-path-not-hash`. Serve: `>= {2, 8}`.
 * - @p hasOldBuiltOutputs gates the legacy `StringMap` JSON fallback used
 *   when @p hasNewBuiltOutputs is false. Worker: `>= {1, 28}`. Serve: `>= {2, 6}`.
 *
 * The status enum is read by the caller; this helper handles only the
 * payload after the status byte.
 */
template<class Proto, class ReadCpuTiming>
inline BuildResult readBuildResult(
    const StoreDirConfig & store,
    typename Proto::ReadConn conn,
    BuildResultStatus status,
    bool hasV2Fields,
    ReadCpuTiming readCpuTiming,
    bool hasNewBuiltOutputs,
    bool hasOldBuiltOutputs)
{
    BuildResult res;
    BuildResult::Success success;

    // Temp variables for failure fields since BuildError uses methods
    std::string errorMsg;
    bool isNonDeterministic = false;

    conn.from >> errorMsg;

    if (hasV2Fields)
        conn.from >> res.timesBuilt >> isNonDeterministic >> res.startTime >> res.stopTime;

    readCpuTiming(res);

    if (hasNewBuiltOutputs) {
        success.builtOutputs = Proto::template Serialise<std::map<OutputName, UnkeyedRealisation>>::read(store, conn);
    } else if (hasOldBuiltOutputs) {
        for (auto && [output, realisation] : Proto::template Serialise<StringMap>::read(store, conn)) {
            size_t n = output.find("!");
            if (n == output.npos)
                throw Error("Invalid derivation output id %s", output);
            success.builtOutputs.insert_or_assign(
                output.substr(n + 1),
                UnkeyedRealisation{
                    StorePath{getString(valueAt(getObject(nlohmann::json::parse(realisation)), "outPath"))}});
        }
    }

    res.inner = std::visit(
        overloaded{
            [&](BuildResult::Success::Status s) -> decltype(res.inner) {
                success.status = s;
                return std::move(success);
            },
            [&](BuildResult::Failure::Status s) -> decltype(res.inner) {
                return BuildResult::Failure{{
                    .status = s,
                    .msg = HintFmt(std::move(errorMsg)),
                    .isNonDeterministic = isNonDeterministic,
                }};
            },
        },
        status);

    return res;
}

/**
 * Write the BuildResult body shared between WorkerProto and ServeProto.
 *
 * Predicate parameters mirror @ref readBuildResult. The cpu-timing
 * write hook fires between the V2 tuple and the builtOutputs block;
 * see @ref readBuildResult for why it is a hook rather than a bool.
 *
 * The protocol predates the use of sum types (std::variant) to separate
 * the success or failure cases. As such, it transits some success- or
 * failure-only fields in both cases; the visitor below passes default
 * values for the fields that don't exist in the variant arm being
 * serialised.
 */
template<class Proto, class WriteCpuTiming>
inline void writeBuildResult(
    const StoreDirConfig & store,
    typename Proto::WriteConn conn,
    const BuildResult & res,
    bool hasV2Fields,
    WriteCpuTiming writeCpuTiming,
    bool hasNewBuiltOutputs,
    bool hasOldBuiltOutputs)
{
    auto common = [&](std::string_view errorMsg, bool isNonDeterministic, const auto & builtOutputs) {
        conn.to << errorMsg;

        if (hasV2Fields)
            conn.to << res.timesBuilt << isNonDeterministic << res.startTime << res.stopTime;

        writeCpuTiming(res);

        if (hasNewBuiltOutputs) {
            Proto::write(store, conn, builtOutputs);
        } else if (hasOldBuiltOutputs) {
            // Old clients read `builtOutputs` as a `StringMap` keyed
            // by `sha256:<hex>!<outputName>` with JSON-encoded
            // realisations. The derivation hash no longer exists, but
            // old clients only extract `outputName` and `outPath`, so
            // a dummy hash suffices.
            StringMap sm;
            for (auto & [outputName, realisation] : builtOutputs) {
                auto dummyId = Hash::dummy.to_string(HashFormat::Base16, true) + "!" + outputName;
                nlohmann::json j;
                j["id"] = dummyId;
                j["outPath"] = realisation.outPath.to_string();
                sm[dummyId] = j.dump();
            }
            Proto::write(store, conn, sm);
        }
    };

    std::visit(
        overloaded{
            [&](const BuildResult::Failure & failure) {
                Proto::write(store, conn, BuildResultStatus{failure.status});
                common(failure.message(), failure.isNonDeterministic, decltype(BuildResult::Success::builtOutputs){});
            },
            [&](const BuildResult::Success & success) {
                Proto::write(store, conn, BuildResultStatus{success.status});
                common(/*errorMsg=*/"", /*isNonDeterministic=*/false, success.builtOutputs);
            },
        },
        res.inner);
}

/**
 * Read the UnkeyedRealisation body shared between WorkerProto and
 * ServeProto. The caller validates the protocol-version gate before
 * invocation.
 */
template<class Proto>
inline UnkeyedRealisation readUnkeyedRealisation(const StoreDirConfig & store, typename Proto::ReadConn conn)
{
    auto outPath = Proto::template Serialise<StorePath>::read(store, conn);
    auto signatures = Proto::template Serialise<std::set<Signature>>::read(store, conn);
    return UnkeyedRealisation{
        .outPath = std::move(outPath),
        .signatures = std::move(signatures),
    };
}

/**
 * Write the UnkeyedRealisation body shared between WorkerProto and
 * ServeProto. The caller validates the protocol-version gate before
 * invocation.
 */
template<class Proto>
inline void writeUnkeyedRealisation(
    const StoreDirConfig & store, typename Proto::WriteConn conn, const UnkeyedRealisation & info)
{
    Proto::write(store, conn, info.outPath);
    Proto::write(store, conn, info.signatures);
}

/**
 * Read the DrvOutput body shared between WorkerProto and ServeProto.
 * The caller validates the protocol-version gate before invocation.
 */
template<class Proto>
inline DrvOutput readDrvOutput(const StoreDirConfig & store, typename Proto::ReadConn conn)
{
    auto drvPath = Proto::template Serialise<StorePath>::read(store, conn);
    auto outputName = Proto::template Serialise<std::string>::read(store, conn);
    return DrvOutput{
        .drvPath = std::move(drvPath),
        .outputName = std::move(outputName),
    };
}

/**
 * Write the DrvOutput body shared between WorkerProto and ServeProto.
 * The caller validates the protocol-version gate before invocation.
 */
template<class Proto>
inline void writeDrvOutput(const StoreDirConfig & store, typename Proto::WriteConn conn, const DrvOutput & info)
{
    Proto::write(store, conn, info.drvPath);
    Proto::write(store, conn, info.outputName);
}

/**
 * Read the Realisation body shared between WorkerProto and ServeProto.
 * The DrvOutput and UnkeyedRealisation reads each go through the
 * protocol's full Serialise<T>::read so per-protocol gates run.
 */
template<class Proto>
inline Realisation readRealisation(const StoreDirConfig & store, typename Proto::ReadConn conn)
{
    auto id = Proto::template Serialise<DrvOutput>::read(store, conn);
    auto unkeyed = Proto::template Serialise<UnkeyedRealisation>::read(store, conn);
    return Realisation{
        std::move(unkeyed),
        std::move(id),
    };
}

/**
 * Write the Realisation body shared between WorkerProto and ServeProto.
 */
template<class Proto>
inline void writeRealisation(const StoreDirConfig & store, typename Proto::WriteConn conn, const Realisation & info)
{
    Proto::write(store, conn, info.id);
    Proto::write(store, conn, static_cast<const UnkeyedRealisation &>(info));
}

} // namespace nix
