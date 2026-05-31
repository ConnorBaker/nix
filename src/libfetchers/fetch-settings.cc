#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/environment-variables.hh"

namespace nix::fetchers {

Settings::Settings()
{
    /* `git-lazy-fetch` defaults to the `NIX_GIT_LAZY_FETCH` env var so
       it can be enabled process-wide without editing nix.conf. An
       explicit config setting still wins: `setDefault` is a no-op once
       the value has been `overridden` by the configuration loader. */
    if (getEnv("NIX_GIT_LAZY_FETCH") == "1")
        gitLazyFetch.setDefault(true);
}

} // namespace nix::fetchers
