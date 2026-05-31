#pragma once
///@file

#include "nix/util/ref.hh"
#include "nix/util/sync.hh"
#include "nix/util/url.hh"
#include "nix/util/processes.hh"
#include "nix/util/file-system.hh"

namespace nix {

OsStrings getNixSshOpts();

class SSHMaster
{
private:

    ParsedURL::Authority authority;
    std::string hostnameAndUser;
    bool fakeSSH;
    const std::optional<std::filesystem::path> keyFile;
    /**
     * Raw bytes, not Base64 encoding.
     */
    const std::string sshPublicHostKey;
    const bool useMaster;
    const bool compress;
    const Descriptor logFD;

    /**
     * Extra ssh options applied to BOTH the control-master connection and every
     * multiplexed session (via `addCommonSSHOpts`). This matters for options
     * that OpenSSH only honours at master-creation time — notably
     * `-oSetEnv=…` / environment forwarding: a session multiplexed over an
     * existing master inherits the master's environment decisions, so a
     * per-session `-oSetEnv` passed only to `startCommand` is silently dropped.
     * Put such options here so they take effect on the master too.
     */
    const OsStrings extraSshArgs;

    const ref<const AutoDelete> tmpDir;

    struct State
    {
#ifndef _WIN32 // TODO re-enable on Windows, once we can start processes.
        Pid sshMaster;
#endif
        std::filesystem::path socketPath;
    };

    Sync<State> state_;

    void addCommonSSHOpts(OsStrings & args);
    bool isMasterRunning();

#ifndef _WIN32 // TODO re-enable on Windows, once we can start processes.
    std::filesystem::path startMaster();
#endif

public:

    SSHMaster(
        const ParsedURL::Authority & authority,
        std::optional<std::filesystem::path> keyFile,
        std::string_view sshPublicHostKey,
        bool useMaster,
        bool compress,
        Descriptor logFD = INVALID_DESCRIPTOR,
        OsStrings extraSshArgs = {});

    struct Connection
    {
#ifndef _WIN32 // TODO re-enable on Windows, once we can start processes.
        Pid sshPid;
#endif
        AutoCloseFD out, in;

        /**
         * Try to set the buffer size in both directions to the
         * designated amount, if possible. If not possible, does
         * nothing.
         *
         * Current implementation is to use `fcntl` with `F_SETPIPE_SZ`,
         * which is Linux-only. For this implementation, `size` must
         * convertible to an `int`. In other words, it must be within
         * `[0, INT_MAX]`.
         */
        void trySetBufferSize(size_t size);
    };

    /**
     * @param command The command (arg vector) to execute.
     *
     * @param extraSshArgs Extra arguments to pass to SSH (not the command to
     * execute). Will not be used when "fake SSHing" to the local
     * machine.
     */
    std::unique_ptr<Connection> startCommand(OsStrings && command, OsStrings && extraSshArgs = {});
};

} // namespace nix
