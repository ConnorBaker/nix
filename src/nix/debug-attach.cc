#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/store/build-debugger.hh"
#include "nix/store/machines.hh"
#include "nix/store/store-api.hh"
#include "nix/store/globals.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/file-system.hh"
#include "nix/util/logging.hh"
#include "nix/util/processes.hh"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#ifndef _WIN32
#  include <csignal>
#  include <fcntl.h>
#  include <grp.h>
#  include <sched.h>
#  include <sstream>
#  include <sys/file.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace nix {

namespace {

#ifdef __linux__

std::filesystem::path debuggerDir()
{
    return std::filesystem::path(settings.nixStateDir) / "debugger";
}

/**
 * Extract the hash part (first 32 base-32 chars) of a /nix/store/… path.
 * We don't rely on the full `Store` / `StorePath` machinery because this
 * subcommand is usable against store paths whose real store isn't
 * accessible locally (e.g. when redirecting to a remote via `--on`).
 */
std::string hashPartOf(std::string_view path)
{
    constexpr size_t hashLen = 32;
    // Nix's store-path base-32 alphabet (see `libutil/hash.cc`).
    constexpr std::string_view base32 = "0123456789abcdfghijklmnpqrsvwxyz";

    auto slash = path.rfind('/');
    auto basename = slash == std::string_view::npos ? path : path.substr(slash + 1);
    if (basename.size() < hashLen || basename[hashLen] != '-')
        throw UsageError(
            "`%s` is not a valid /nix/store path (expected `<32-char-hash>-name.drv`)",
            std::string{path});

    auto hash = basename.substr(0, hashLen);
    for (char c : hash)
        if (base32.find(c) == std::string_view::npos)
            throw UsageError(
                "`%s` is not a valid /nix/store path: hash part contains `%c` which is not in the base-32 alphabet",
                std::string{path}, c);

    return std::string{hash};
}

std::filesystem::path attachInfoPathFor(std::string_view drvPath)
{
    return debuggerDir() / (hashPartOf(drvPath) + ".attach");
}

std::filesystem::path lockPathFor(std::string_view drvPath)
{
    return debuggerDir() / (hashPartOf(drvPath) + ".lock");
}

/**
 * Read and parse the attach-info file. Returns nullopt if absent (build
 * hasn't started or already finished). Throws if the file is present but
 * malformed or speaks a newer schema version than we understand.
 */
std::optional<nlohmann::json> tryReadAttachInfo(const std::filesystem::path & p)
{
    std::error_code ec;
    if (!std::filesystem::exists(p, ec))
        return std::nullopt;
    auto content = readFile(p.string());
    auto j = nlohmann::json::parse(content);

    auto version = j.value<int>("schemaVersion", 0);
    if (version == 0)
        throw Error(
            "build-debugger attach-info `%s` is missing `schemaVersion` — "
            "produced by a Nix that predates this subcommand's expectations. "
            "Re-run the build with a matching Nix (or delete the stale file "
            "if the build is no longer running).",
            p.string());
    if (version > kDebuggerAttachInfoSchemaVersion)
        throw Error(
            "build-debugger attach-info `%s` uses schema version %d, but "
            "this `nix debug-attach` only understands up to %d. Upgrade "
            "your local Nix (e.g. `nix upgrade-nix` or `nix profile upgrade`) "
            "to match the daemon that wrote the file, or delete the file "
            "if the build is no longer running.",
            p.string(), version, kDebuggerAttachInfoSchemaVersion);
    return j;
}

/**
 * Read the first line of /proc/<pid>/cmdline (args are NUL-separated, with
 * a trailing NUL) and return true iff any of the args contain `needle`.
 * Used to sanity-check that the target pid is actually our wrapper before
 * `nsenter`ing — a reused pid belonging to an unrelated process would be
 * catastrophic.
 */
bool cmdlineContains(pid_t pid, std::string_view needle)
{
    auto path = "/proc/" + std::to_string(pid) + "/cmdline";
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return false;
    std::string buf((std::istreambuf_iterator<char>(f)), {});
    for (auto & c : buf)
        if (c == '\0')
            c = ' ';
    return buf.find(needle) != std::string::npos;
}

/**
 * Open `/proc/<targetPid>/ns/<ns>` for setns. Opening before any setns
 * calls avoids the two failure modes of interleaved lookups:
 *   1. After `setns(CLONE_NEWUSER)` we may lose the caps needed to
 *      re-read /proc entries for `targetPid`.
 *   2. After `setns(CLONE_NEWPID)` the outside pid is invisible.
 */
AutoCloseFD openNsFd(pid_t targetPid, const char * ns)
{
    auto path = "/proc/" + std::to_string(targetPid) + "/ns/" + ns;
    AutoCloseFD fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (!fd)
        throw SysError("opening `%s`", path);
    return fd;
}

void setnsOrThrow(int fd, const char * label)
{
    if (::setns(fd, 0) < 0)
        throw SysError("setns(%s)", label);
}

/**
 * Read the effective uid/gid of `targetPid` from /proc/<pid>/status as
 * seen from our current vantage point. Must be called AFTER entering
 * the target's user namespace (so the uids appear in the target's
 * mapping — the "follow" semantics of `nsenter --setuid follow`) and
 * BEFORE entering the target's pid namespace (after that, the outside
 * `targetPid` no longer resolves in /proc).
 *
 * /proc/<pid>/status's Uid line is `Uid:\treal\teffective\tsaved\tfs`.
 */
std::pair<uid_t, gid_t> readTargetUidGid(pid_t targetPid)
{
    auto path = "/proc/" + std::to_string(targetPid) + "/status";
    std::ifstream f(path);
    if (!f.is_open())
        throw SysError("opening `%s`", path);
    std::optional<uid_t> euid;
    std::optional<gid_t> egid;
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("Uid:")) {
            std::istringstream iss(line.substr(4));
            uid_t real, eff;
            if (iss >> real >> eff)
                euid = eff;
        } else if (line.starts_with("Gid:")) {
            std::istringstream iss(line.substr(4));
            gid_t real, eff;
            if (iss >> real >> eff)
                egid = eff;
        }
        if (euid && egid)
            break;
    }
    if (!euid || !egid)
        throw Error("could not parse uid/gid from `%s`", path);
    return {*euid, *egid};
}

/**
 * Open (create if absent) the attach-lock file at `lockPath`. The fd is
 * separate from lock acquisition so callers can report "couldn't even
 * open the file" differently from "file is already flock'd".
 */
AutoCloseFD openAttachLockFile(const std::filesystem::path & lockPath)
{
    AutoCloseFD fd{open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600)};
    if (!fd)
        throw SysError("opening attach lock `%s`", lockPath.string());
    return fd;
}

/**
 * Try to take an exclusive non-blocking flock on an already-opened lock
 * fd. Returns true iff the lock was acquired. A false return means
 * another process holds it; a real I/O failure still throws.
 */
bool tryAcquireAttachLock(int fd, const std::filesystem::path & lockPath)
{
    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        return true;
    if (errno != EWOULDBLOCK)
        throw SysError("acquiring attach lock `%s`", lockPath.string());
    return false;
}

#endif // __linux__

} // namespace

struct CmdDebugAttach : Command
{
    std::string drvPathArg;
    std::optional<std::string> remoteHostOverride;
    bool force = false;

    CmdDebugAttach()
    {
        expectArg("drv-path", &drvPathArg);

        addFlag({
            .longName = "on",
            .description =
                "Run `nix debug-attach` on the specified host via SSH "
                "instead of locally. Use for builds dispatched to a remote "
                "builder; the remote host is the one the build is actually "
                "running on.",
            .labels = {"host"},
            .handler = {[this](std::string host) {
                if (host.empty())
                    throw UsageError("`--on` requires a non-empty host");
                remoteHostOverride = std::move(host);
            }},
        });

        addFlag({
            .longName = "force",
            .description =
                "Attach even if another `nix debug-attach` session currently "
                "holds the lock for this build. Use when the previous attach "
                "process is stuck or has died without releasing its lock.",
            .handler = {&force, true},
        });
    }

    std::string description() override
    {
        return "attach an interactive shell to a build paused by `--build-debugger`";
    }

    std::string doc() override
    {
        return
#include "debug-attach.md"
            ;
    }

    void run() override
    {
        experimentalFeatureSettings.require(Xp::BuildDebugger);

#ifndef __linux__
        throw UsageError(
            "`nix debug-attach` is Linux-only (it enters the failed build "
            "sandbox via Linux namespaces using `nsenter`)");
#else
        // If the caller explicitly targeted a remote, short-circuit to SSH.
        // The root check below does NOT apply because ssh itself will
        // gate the privilege on the remote via the explicit `sudo`.
        if (remoteHostOverride) {
            dispatchRemote(*remoteHostOverride, drvPathArg);
            return;
        }

        // Validate the drv-path shape early so we give a clear error on
        // bogus input (independent of attach-info presence).
        (void) hashPartOf(drvPathArg);

        // Otherwise look for a local attach-info. If we find a redirect to
        // a remote host, follow it automatically. Else attach locally.
        auto attachFile = attachInfoPathFor(drvPathArg);
        if (auto info = tryReadAttachInfo(attachFile)) {
            if (auto remote = info->value<std::string>("remoteHost", ""); !remote.empty()) {
                printInfo("build `%s` was dispatched to remote `%s`; re-running over SSH",
                          drvPathArg, remote);
                dispatchRemote(remote, drvPathArg);
                return;
            }
        }

        if (geteuid() != 0)
            throw UsageError(
                "`nix debug-attach` must be run as root (it invokes `nsenter` "
                "to enter the build sandbox's Linux namespaces). Re-run with: "
                "`sudo nix debug-attach %s`",
                drvPathArg);

        std::filesystem::create_directories(debuggerDir());
        auto lockPath = lockPathFor(drvPathArg);
        auto lockFd = openAttachLockFile(lockPath);
        if (!tryAcquireAttachLock(lockFd.get(), lockPath)) {
            if (!force)
                throw UsageError(
                    "another `nix debug-attach` session is already running for this "
                    "build (lock held on `%s`). Wait for it to finish, or pass --force "
                    "to attach anyway",
                    lockPath.string());
            logWarning({.msg = HintFmt(
                "attach lock `%s` is held by another session; proceeding without "
                "holding the lock (--force). Multiple attach sessions on the same "
                "paused build may fight over the PTY",
                lockPath.string())});
        }

        auto info = tryReadAttachInfo(attachFile);
        if (!info)
            throw UsageError(
                "build `%s` is not currently paused by `--build-debugger`. "
                "Run `nix build --build-debugger %s` in another terminal and "
                "follow the `sudo nix debug-attach` instruction it prints on "
                "failure",
                drvPathArg, drvPathArg);

        auto sandboxTmp = info->value<std::string>("sandboxTmpdir", "");
        if (sandboxTmp.empty())
            throw Error(
                "attach-info file `%s` is missing `sandboxTmpdir`",
                attachFile.string());
        auto hostTmp = info->value<std::string>("hostTmpdir", "");
        auto envVarsOnHost = hostTmp.empty()
            ? std::filesystem::path(sandboxTmp) / "env-vars"
            : std::filesystem::path(hostTmp) / "env-vars";
        auto pidRaw = info->value<pid_t>("pid", pid_t{0});

        // Open the pidfd FIRST, then use `pidfdAlive` for the liveness
        // probe instead of `kill(pid, 0)`. `kill(pid, 0)` is race-prone:
        // between the kill check and the subsequent setns/exec, the pid
        // can exit and be reused for an unrelated process. The pidfd
        // pins the task_struct, so `pidfd_send_signal(fd, 0, …)`
        // correctly reports ESRCH once the original task exits even if
        // the pid number has been reassigned.
        AutoCloseFD pidfd{pidRaw > 0 ? openPidfd(pidRaw) : -1};

        // Attach-info is published at `startBuild`; env-vars is written
        // only when the wrapper's EXIT trap fires. If env-vars is
        // missing, the build is either still running (and hasn't hit
        // its failure point yet) or the wrapper was replaced mid-build
        // by `exec` in the sourced builder — the EXIT trap is lost with
        // the replaced process image and no amount of waiting will
        // produce an env-vars file.
        //
        // Distinguish the two cases: a dead wrapper pid + absent
        // env-vars means `exec` (or external kill / daemon shutdown)
        // ate our trap. Point users at that distinct failure mode
        // instead of telling them to wait forever.
        //
        // "Dead pid" here covers both "we opened a pidfd and it now
        // reports ESRCH" (pid was alive at open time, died before the
        // check) AND "pidfd_open itself returned -1 because the pid
        // was already gone" (wrapper exited before we even got here,
        // which is the common `exec`-chain case). For the latter we
        // confirm via `kill(pid, 0) == ESRCH` to filter out pidfd_open
        // failures that aren't about liveness (EACCES on locked-down
        // kernels, etc.). On pre-5.3 kernels with no pidfd at all, we
        // rely solely on kill — which is race-prone for pid reuse but
        // acceptable here since this is only the user-facing error
        // message, not the security-critical attach gate.
        bool pidfdKnownDead = pidfd.get() >= 0 && !pidfdAlive(pidfd.get());
        bool killSaysDead = pidRaw > 0 && ::kill(pidRaw, 0) < 0 && errno == ESRCH;
        bool pidDead = pidfdKnownDead || killSaysDead;
        bool envVarsAbsent = !std::filesystem::exists(envVarsOnHost);
        if (pidRaw <= 0 || envVarsAbsent || pidDead) {
            if (pidDead && envVarsAbsent)
                // The pid is gone AND env-vars was never written. The
                // orthogonal causes are:
                //   a) `exec` in the sourced builder replaced the wrapper
                //      mid-script — the common case.
                //   b) The wrapper was SIGKILLed (or crashed) before
                //      reaching the env-vars capture block.
                // Other paths — the 1-hour pause expiring, user hitting
                // Ctrl-C in nix build, daemon shutdown after env-vars is
                // written — would leave env-vars present, so they're not
                // listed here; they'd not reach this diagnostic.
                throw UsageError(
                    "build `%s` has attach-info but the wrapper pid %d has "
                    "already exited without writing `env-vars` — the wrapper "
                    "died before reaching the failure-capture path. Most "
                    "likely cause: an `exec` in the sourced builder script "
                    "replaced the wrapper's bash (and with it, the `EXIT` "
                    "trap and `failureHook` function) with another program. "
                    "`--build-debugger` cannot pause a build once `exec` has "
                    "chained away from the wrapper; run the exec'd command "
                    "as a child process instead.\n\nAlternatively, the "
                    "wrapper may have been SIGKILLed (by the OOM killer or "
                    "similar) before it reached its failure-capture block, "
                    "in which case the original failure is in the build log "
                    "and there is nothing for the debugger to attach to.",
                    drvPathArg, pidRaw);
            throw UsageError(
                "build `%s` is not paused at its failure point yet. The "
                "sandbox's `env-vars` file hasn't been written, which "
                "means the wrapper's EXIT trap hasn't fired. Wait for "
                "the failure message to appear in the `nix build` log "
                "and then re-run `sudo nix debug-attach %s`",
                drvPathArg, drvPathArg);
        }

        auto envVarsFileInSandbox = std::filesystem::path(sandboxTmp) / "env-vars";
        auto bashPath = info->at("bash").get<std::string>();

        // Defence-in-depth against pid reuse / stale attach-info: verify
        // the target process is still our wrapper script by name. On
        // Linux >= 5.3 we also hold a pidfd across the nsenter call.
        if (!cmdlineContains(pidRaw, ".nix-debug-wrapper.sh"))
            throw Error(
                "process %d (the build-debugger wrapper recorded in `%s`) "
                "no longer runs the expected wrapper script. The build "
                "probably exited (or its pid was reused); refusing to "
                "nsenter into an unrelated process.",
                pidRaw, attachFile.string());

        execAttach(pidRaw, pidfd.get(), bashPath, envVarsFileInSandbox);

        // Wake the wrapper: its `wait` is SIGTERM-trapped to resume.
        // ESRCH is fine — the wrapper may have already timed out.
        if (::kill(pidRaw, SIGTERM) < 0 && errno != ESRCH)
            throw SysError("signaling build-debugger wrapper (pid %d)", pidRaw);
#endif
    }

#ifdef __linux__

private:
    /**
     * Parse a [user@]host[:port] authority. Returns (user, host, port)
     * where user and port may be empty.
     */
    static std::tuple<std::string, std::string, std::string>
    splitSshAuthority(std::string_view authority)
    {
        std::string user, host, port;
        auto at = authority.find('@');
        if (at != std::string_view::npos) {
            user = std::string(authority.substr(0, at));
            authority = authority.substr(at + 1);
        }
        auto colon = authority.find(':');
        if (colon != std::string_view::npos) {
            host = std::string(authority.substr(0, colon));
            port = std::string(authority.substr(colon + 1));
        } else {
            host = std::string(authority);
        }
        return {user, host, port};
    }

    /**
     * Look the target up in `nix.buildMachines`. Accepts either a bare
     * hostname (as typed via `--on HOST`) or a full store URI (the form
     * a redirect attach-info records in `remoteHost`). Returns nullopt
     * if the host isn't listed — the caller falls back to a bare `ssh`.
     */
    std::optional<Machine> findConfiguredMachine(std::string_view hostSpec)
    {
        std::string effective(hostSpec);
        for (auto * prefix : {"ssh://", "ssh-ng://"}) {
            auto p = std::string_view{prefix};
            if (effective.starts_with(p)) {
                effective.erase(0, p.size());
                break;
            }
        }

        Machines machines;
        try {
            machines = Machine::parseConfig(
                {settings.thisSystem},
                settings.getWorkerSettings().builders);
        } catch (std::exception & e) {
            debug("build-debugger: failed to parse `nix.buildMachines`: %s", e.what());
            return std::nullopt;
        }

        for (auto & m : machines) {
            auto * spec = std::get_if<StoreReference::Specified>(&m.storeUri.variant);
            if (!spec)
                continue;
            if (spec->scheme != "ssh" && spec->scheme != "ssh-ng")
                continue;
            auto [mUser, mHost, mPort] = splitSshAuthority(spec->authority);
            if (mHost == effective || spec->authority == effective)
                return m;
        }
        return std::nullopt;
    }

    void dispatchRemote(const std::string & host, const std::string & drvPath)
    {
        // `ssh -t` allocates a TTY on the remote so interactive bash works.
        // We don't pass --extra-experimental-features — if the remote's
        // Nix doesn't have `build-debugger` enabled, it should error
        // clearly with its own message rather than silently accepting
        // experimental surface from our client.
        std::vector<std::string> storage = {"ssh", "-t"};
        std::string displayHost = host;
        bool explicitRoot = false;

        if (auto machine = findConfiguredMachine(host)) {
            auto * spec = std::get_if<StoreReference::Specified>(&machine->storeUri.variant);
            auto [mUser, mHost, mPort] = splitSshAuthority(spec->authority);

            if (!mPort.empty()) {
                storage.emplace_back("-p");
                storage.emplace_back(mPort);
            }
            if (machine->sshKey) {
                storage.emplace_back("-i");
                storage.emplace_back(machine->sshKey->string());
            }
            // Don't pass `-o BatchMode=yes` here. Unlike the build hook
            // (which must never prompt a daemon), `nix debug-attach` is
            // invoked by a user at a terminal — a first-contact host-key
            // prompt is friendlier than a "Host key verification failed"
            // dead-end.

            displayHost = mUser.empty() ? mHost : mUser + "@" + mHost;
            explicitRoot = mUser == "root";
            storage.emplace_back(displayHost);
            debug(
                "build-debugger: dispatching to `%s` via configured nix.buildMachines entry",
                displayHost);
        } else {
            // Fallback: bare ssh against whatever the user passed. Keeps
            // the pre-configuration-lookup behavior for users who don't
            // use `nix.buildMachines`.
            storage.emplace_back(host);
            debug(
                "build-debugger: `%s` not in `nix.buildMachines`; dispatching via bare ssh",
                host);
        }

        storage.emplace_back("--");
        if (!explicitRoot)
            storage.emplace_back("sudo");
        storage.emplace_back("nix");
        storage.emplace_back("debug-attach");
        storage.emplace_back(drvPath);

        std::vector<char *> args;
        args.reserve(storage.size() + 1);
        for (auto & s : storage)
            args.push_back(const_cast<char *>(s.c_str()));
        args.push_back(nullptr);

        // Fork + exec so we can wrap SSH's exit status into a nicer error.
        pid_t child = ::fork();
        if (child < 0)
            throw SysError("fork");
        if (child == 0) {
            ::execvp("ssh", args.data());
            std::fprintf(
                stderr, "exec ssh failed: %s\n", std::strerror(errno));
            ::_exit(127);
        }

        int status = 0;
        while (true) {
            pid_t r = ::waitpid(child, &status, 0);
            if (r == child)
                break;
            if (r < 0 && errno != EINTR)
                throw SysError("waitpid");
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            auto rc = WEXITSTATUS(status);
            // 255 = SSH itself failed to connect. 127 = our exec ssh failed.
            // Otherwise = remote command exit code.
            if (rc == 255)
                throw Error(
                    "SSH connection to `%s` failed. Check `ssh %s` works "
                    "interactively, that the host is reachable, and that "
                    "your SSH keys are configured.",
                    host, host);
            if (rc == 127)
                throw Error("`ssh` binary not found in PATH");
            throw Error(
                "`nix debug-attach` on remote `%s` exited with status %d. "
                "Check that the remote has the `build-debugger` feature "
                "enabled and that its Nix is new enough.",
                host, rc);
        }
        if (WIFSIGNALED(status))
            throw Error(
                "`ssh` to `%s` was killed by signal %d",
                host, WTERMSIG(status));
    }

    void execAttach(
        pid_t pidRaw,
        int pidfd,
        const std::string & bashPath,
        const std::filesystem::path & envVarsFileInSandbox)
    {
        // Final liveness check right before fork. The pidfd is the
        // race-free handle — if the target died, this reports it.
        if (pidfd >= 0 && !pidfdAlive(pidfd))
            throw Error(
                "build-debugger wrapper pid %d exited before we could attach "
                "(pidfd reports ESRCH). The paused session ended — re-run the "
                "build to try again.",
                pidRaw);
        if (pidfd < 0)
            throw Error(
                "race-free pid tracking requires Linux ≥ 5.3 (pidfd_open); "
                "refusing to attach without a pidfd — a pid-reuse race between "
                "now and the setns call could land us in an unrelated process");

        pid_t child = ::fork();
        if (child < 0)
            throw SysError("fork");
        if (child == 0) {
            // Child: enter the target's namespaces step-by-step. The
            // sequence is delicate — we must:
            //   1. Open every /proc/<target>/ns/<ns> fd FIRST, while we
            //      still have root privileges in our parent namespace
            //      (some of these lookups EPERM after we enter the
            //      target's user ns with a remapped uid).
            //   2. setns(user) — after this /proc/<target>/status's
            //      Uid/Gid appear in the target's uid-map (inside-ns
            //      uids, e.g. 0 for nixbld-root), matching the "follow"
            //      semantics of `nsenter --setuid follow`.
            //   3. Read those uids BEFORE entering the pid namespace
            //      (once we setns(pid), the outside-pid is invisible
            //      via /proc).
            //   4. setns(mnt/ipc/uts/net/pid).
            //   5. Drop creds (setgroups → setgid → setuid).
            //   6. fork() — CLONE_NEWPID takes effect for children.
            //   7. execv bash in the grandchild.
            //
            // The pidfd remains open across all of the above so a
            // mid-sequence pid reuse still aborts at the final
            // pidfd_send_signal(fd, 0, …) liveness check.
            try {
                auto userFd = openNsFd(pidRaw, "user");
                auto mntFd = openNsFd(pidRaw, "mnt");
                auto ipcFd = openNsFd(pidRaw, "ipc");
                auto utsFd = openNsFd(pidRaw, "uts");
                auto netFd = openNsFd(pidRaw, "net");
                auto pidFdNs = openNsFd(pidRaw, "pid");

                setnsOrThrow(userFd.get(), "user");
                auto [uid, gid] = readTargetUidGid(pidRaw);
                setnsOrThrow(mntFd.get(), "mnt");
                setnsOrThrow(ipcFd.get(), "ipc");
                setnsOrThrow(utsFd.get(), "uts");
                setnsOrThrow(pidFdNs.get(), "pid");
                setnsOrThrow(netFd.get(), "net");

                // setgroups() must precede setgid/setuid so loss of
                // caps later can't block it. EPERM is tolerated for
                // sandboxes that disabled setgroups.
                if (::setgroups(0, nullptr) < 0 && errno != EPERM)
                    throw SysError("setgroups(0, NULL)");
                if (::setgid(gid) < 0)
                    throw SysError("setgid(%d)", gid);
                if (::setuid(uid) < 0)
                    throw SysError("setuid(%d)", uid);
            } catch (std::exception & e) {
                std::fprintf(stderr, "nix debug-attach: %s\n", e.what());
                ::_exit(127);
            }

            pid_t grandchild = ::fork();
            if (grandchild < 0) {
                std::fprintf(stderr, "nix debug-attach: fork after setns: %s\n",
                             std::strerror(errno));
                ::_exit(127);
            }
            if (grandchild == 0) {
                std::vector<std::string> storage = {
                    bashPath,
                    "--init-file", envVarsFileInSandbox.string(),
                    "-i",
                };
                std::vector<char *> args;
                args.reserve(storage.size() + 1);
                for (auto & s : storage)
                    args.push_back(const_cast<char *>(s.c_str()));
                args.push_back(nullptr);
                ::execv(bashPath.c_str(), args.data());
                std::fprintf(stderr, "nix debug-attach: exec `%s` failed: %s\n",
                             bashPath.c_str(), std::strerror(errno));
                ::_exit(127);
            }

            int innerStatus = 0;
            while (true) {
                pid_t r = ::waitpid(grandchild, &innerStatus, 0);
                if (r == grandchild)
                    break;
                if (r < 0 && errno != EINTR) {
                    std::fprintf(stderr, "nix debug-attach: waitpid: %s\n",
                                 std::strerror(errno));
                    ::_exit(127);
                }
            }
            if (WIFEXITED(innerStatus))
                ::_exit(WEXITSTATUS(innerStatus));
            if (WIFSIGNALED(innerStatus))
                ::_exit(128 + WTERMSIG(innerStatus));
            ::_exit(127);
        }

        int status = 0;
        while (true) {
            pid_t r = ::waitpid(child, &status, 0);
            if (r == child)
                break;
            if (r < 0 && errno != EINTR)
                throw SysError("waitpid");
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            logWarning(
                {.msg = HintFmt(
                     "attach session exited with status %d", WEXITSTATUS(status))});
    }

#endif // __linux__
};

static auto rCmdDebugAttach = registerCommand<CmdDebugAttach>("debug-attach");

} // namespace nix
