#include "nix/util/serialise.hh"
#include "nix/store/path-with-outputs.hh"
#include "nix/store/store-api.hh"
#include "nix/store/build-result.hh"
#include "nix/store/common-protocol.hh"
#include "nix/store/common-protocol-impl.hh"
#include "nix/store/serve-protocol.hh"
#include "nix/store/serve-protocol-impl.hh"
#include "nix/store/path-info.hh"
#include "nix/util/json-utils.hh"

#include <nlohmann/json.hpp>

namespace nix {

/* protocol-specific definitions */

BuildResult ServeProto::Serialise<BuildResult>::read(const StoreDirConfig & store, ServeProto::ReadConn conn)
{
    auto status = ServeProto::Serialise<BuildResultStatus>::read(store, conn);
    return readBuildResult<ServeProto>(
        store,
        conn,
        status,
        /*hasV2Fields=*/conn.version >= ServeProto::Version{2, 3},
        /*readCpuTiming=*/[](BuildResult &) {}, // serve protocol does not carry cpu timing
        /*hasNewBuiltOutputs=*/conn.version >= ServeProto::Version{2, 8},
        /*hasOldBuiltOutputs=*/conn.version >= ServeProto::Version{2, 6});
}

void ServeProto::Serialise<BuildResult>::write(
    const StoreDirConfig & store, ServeProto::WriteConn conn, const BuildResult & res)
{
    writeBuildResult<ServeProto>(
        store,
        conn,
        res,
        /*hasV2Fields=*/conn.version >= ServeProto::Version{2, 3},
        /*writeCpuTiming=*/[](const BuildResult &) {}, // serve protocol does not carry cpu timing
        /*hasNewBuiltOutputs=*/conn.version >= ServeProto::Version{2, 8},
        /*hasOldBuiltOutputs=*/conn.version >= ServeProto::Version{2, 6});
}

UnkeyedValidPathInfo ServeProto::Serialise<UnkeyedValidPathInfo>::read(const StoreDirConfig & store, ReadConn conn)
{
    /* Hash should be set below unless very old `nix-store --serve`.
       Caller should assert that it did set it. */
    UnkeyedValidPathInfo info{store, Hash::dummy};

    auto deriver = readString(conn.from);
    if (deriver != "")
        info.deriver = store.parseStorePath(deriver);
    info.references = ServeProto::Serialise<StorePathSet>::read(store, conn);

    readLongLong(conn.from); // download size, unused
    info.narSize = readLongLong(conn.from);

    if (conn.version >= ServeProto::Version{2, 4}) {
        auto s = readString(conn.from);
        if (!s.empty())
            info.narHash = Hash::parseAnyPrefixed(s);
        info.ca = ContentAddress::parseOpt(readString(conn.from));
        info.sigs = ServeProto::Serialise<std::set<Signature>>::read(store, conn);
    }

    return info;
}

void ServeProto::Serialise<UnkeyedValidPathInfo>::write(
    const StoreDirConfig & store, WriteConn conn, const UnkeyedValidPathInfo & info)
{
    conn.to << (info.deriver ? store.printStorePath(*info.deriver) : "");

    ServeProto::write(store, conn, info.references);
    // !!! Maybe we want compression?
    conn.to << info.narSize // downloadSize, lie a little
            << info.narSize;
    if (conn.version >= ServeProto::Version{2, 4}) {
        conn.to << info.narHash.to_string(HashFormat::Nix32, true) << renderContentAddress(info.ca);
        ServeProto::write(store, conn, info.sigs);
    }
}

ServeProto::BuildOptions
ServeProto::Serialise<ServeProto::BuildOptions>::read(const StoreDirConfig & store, ReadConn conn)
{
    BuildOptions options;
    options.maxSilentTime = readInt(conn.from);
    options.buildTimeout = readInt(conn.from);
    if (conn.version >= ServeProto::Version{2, 2})
        options.maxLogSize = readNum<unsigned long>(conn.from);
    if (conn.version >= ServeProto::Version{2, 3}) {
        options.nrRepeats = readInt(conn.from);
        options.enforceDeterminism = readInt(conn.from);
    }
    if (conn.version >= ServeProto::Version{2, 7}) {
        options.keepFailed = (bool) readInt(conn.from);
    }
    return options;
}

void ServeProto::Serialise<ServeProto::BuildOptions>::write(
    const StoreDirConfig & store, WriteConn conn, const ServeProto::BuildOptions & options)
{
    conn.to << options.maxSilentTime << options.buildTimeout;
    if (conn.version >= ServeProto::Version{2, 2})
        conn.to << options.maxLogSize;
    if (conn.version >= ServeProto::Version{2, 3})
        conn.to << options.nrRepeats << options.enforceDeterminism;

    if (conn.version >= ServeProto::Version{2, 7}) {
        conn.to << ((int) options.keepFailed);
    }
}

UnkeyedRealisation ServeProto::Serialise<UnkeyedRealisation>::read(const StoreDirConfig & store, ReadConn conn)
{
    if (conn.version < ServeProto::Version{2, 8}) {
        throw Error(
            "serve protocol %d.%d is too old (< 2.8) to support content-addressing derivations",
            conn.version.major,
            conn.version.minor);
    }
    return readUnkeyedRealisation<ServeProto>(store, conn);
}

void ServeProto::Serialise<UnkeyedRealisation>::write(
    const StoreDirConfig & store, WriteConn conn, const UnkeyedRealisation & info)
{
    if (conn.version < ServeProto::Version{2, 8}) {
        throw Error(
            "serve protocol %d.%d is too old (< 2.8) to support content-addressing derivations",
            conn.version.major,
            conn.version.minor);
    }
    writeUnkeyedRealisation<ServeProto>(store, conn, info);
}

DrvOutput ServeProto::Serialise<DrvOutput>::read(const StoreDirConfig & store, ReadConn conn)
{
    if (conn.version < ServeProto::Version{2, 8}) {
        throw Error(
            "serve protocol %d.%d is too old (< 2.8) to support content-addressing derivations",
            conn.version.major,
            conn.version.minor);
    }
    return readDrvOutput<ServeProto>(store, conn);
}

void ServeProto::Serialise<DrvOutput>::write(const StoreDirConfig & store, WriteConn conn, const DrvOutput & info)
{
    if (conn.version < ServeProto::Version{2, 8}) {
        throw Error(
            "serve protocol %d.%d is too old (< 2.8) to support content-addressing derivations",
            conn.version.major,
            conn.version.minor);
    }
    writeDrvOutput<ServeProto>(store, conn, info);
}

Realisation ServeProto::Serialise<Realisation>::read(const StoreDirConfig & store, ReadConn conn)
{
    return readRealisation<ServeProto>(store, conn);
}

void ServeProto::Serialise<Realisation>::write(const StoreDirConfig & store, WriteConn conn, const Realisation & info)
{
    writeRealisation<ServeProto>(store, conn, info);
}

} // namespace nix
