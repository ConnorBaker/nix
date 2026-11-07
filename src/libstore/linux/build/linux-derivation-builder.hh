#pragma once

#include "unix-derivation-builder-impl.hh"

#include "nix/store/personality.hh"
#include "store-config-private.hh"

#include <sys/prctl.h>

#if HAVE_LANDLOCK
#  include <linux/landlock.h>
#endif

#if HAVE_LANDLOCK && defined(LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET)
#  define DO_LANDLOCK 1
#else
#  define DO_LANDLOCK 0
#endif

namespace nix {

void setupSeccomp(const LocalSettings & localSettings);

struct LinuxDerivationBuilder : virtual UnixDerivationBuilderImpl
{
    using UnixDerivationBuilderImpl::UnixDerivationBuilderImpl;

    void enterChroot() override;

    void startChild() override;

    /**
     * Start the build child, forked by a fresh exec of Nix, in `mode` with
     * the pty slave as stdout and stderr and `fds` passed on; see
     * runBuildChild().
     */
    Pid spawnBuildChild(std::string_view mode, std::vector<FdRedirection> fds = {});

    /**
     * Serve the build children that spawnBuildChild() starts.
     */
    [[noreturn]] static void runBuildChild();

    [[noreturn]] static void runBuildChildOnce();
};

} // namespace nix
