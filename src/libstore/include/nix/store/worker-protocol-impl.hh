#pragma once
/**
 * @file
 *
 * Template implementations (as opposed to mere declarations).
 *
 * This file is an example of the "impl.hh" pattern. See the
 * contributing guide.
 */

#include "nix/store/worker-protocol.hh"
#include "nix/store/length-prefixed-protocol-helper.hh"

namespace nix {

/* protocol-agnostic templates */

USE_LENGTH_PREFIX_SERIALISERS(WorkerProto)

/**
 * Use `CommonProto` where possible.
 */
template<typename T>
struct WorkerProto::Serialise
{
    static T read(const StoreDirConfig & store, WorkerProto::ReadConn conn)
    {
        return CommonProto::Serialise<T>::read(store, CommonProto::ReadConn{.from = conn.from});
    }

    static void write(const StoreDirConfig & store, WorkerProto::WriteConn conn, const T & t)
    {
        CommonProto::Serialise<T>::write(store, CommonProto::WriteConn{.to = conn.to}, t);
    }
};

/* protocol-specific templates */

} // namespace nix
