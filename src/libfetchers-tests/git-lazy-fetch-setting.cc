/* The `git-lazy-fetch` setting and its `NIX_GIT_LAZY_FETCH` env-var
 * default (PROPOSAL.md §6.3).
 *
 * These pin the precedence contract directly on `fetchers::Settings`,
 * which is the only NON-VACUOUS way to test it: the functional test
 * (`fetchGitLazy.sh`) can only fetch `file://` repos, and the safety
 * gate skips partial-clone creation for non-http(s)/ssh transports —
 * so a `file://` fetch produces an identical result whether the
 * setting is on or off. That proves transparency, not that the env var
 * actually flips the setting. Here we read the setting value straight
 * off a freshly-constructed `Settings`, so the toggle is observable.
 *
 * Contract:
 *   - default is `false`;
 *   - `NIX_GIT_LAZY_FETCH=1` seeds the default to `true`;
 *   - any other env value (including unset / "0" / "true") does NOT
 *     seed it (the constructor matches "1" exactly);
 *   - an explicit config assignment always wins over the env var
 *     (env seeds via `setDefault`, which is a no-op once `overridden`).
 */

#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/environment-variables.hh"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>

namespace nix {

/* Saves `NIX_GIT_LAZY_FETCH` on entry and restores it on exit, so these
   tests don't leak environment state into the rest of the binary
   (gtest runs them in-process with everything else). */
class GitLazyFetchSettingTest : public ::testing::Test
{
    std::optional<std::string> saved;

protected:
    void SetUp() override
    {
        saved = getEnv("NIX_GIT_LAZY_FETCH");
        ::unsetenv("NIX_GIT_LAZY_FETCH");
    }

    void TearDown() override
    {
        if (saved)
            setEnv("NIX_GIT_LAZY_FETCH", saved->c_str());
        else
            ::unsetenv("NIX_GIT_LAZY_FETCH");
    }
};

TEST_F(GitLazyFetchSettingTest, DefaultsToFalseWhenEnvUnset)
{
    fetchers::Settings settings;
    EXPECT_FALSE(settings.gitLazyFetch.get());
}

TEST_F(GitLazyFetchSettingTest, EnvVarOneSeedsTrue)
{
    setEnv("NIX_GIT_LAZY_FETCH", "1");
    fetchers::Settings settings;
    EXPECT_TRUE(settings.gitLazyFetch.get());
}

TEST_F(GitLazyFetchSettingTest, EnvVarZeroLeavesDefaultFalse)
{
    /* Only "1" enables it; "0" must not. */
    setEnv("NIX_GIT_LAZY_FETCH", "0");
    fetchers::Settings settings;
    EXPECT_FALSE(settings.gitLazyFetch.get());
}

TEST_F(GitLazyFetchSettingTest, EnvVarOtherValueLeavesDefaultFalse)
{
    /* The constructor matches "1" exactly — "true"/"yes" do NOT seed it
       (mirrors how other env-seeded bool settings in the codebase are
       written, e.g. `NIX_IGNORE_SYMLINK_STORE`). */
    setEnv("NIX_GIT_LAZY_FETCH", "true");
    fetchers::Settings settings;
    EXPECT_FALSE(settings.gitLazyFetch.get());
}

TEST_F(GitLazyFetchSettingTest, ExplicitConfigTrueWinsWhenEnvUnset)
{
    fetchers::Settings settings;
    ASSERT_TRUE(settings.set("git-lazy-fetch", "true"));
    EXPECT_TRUE(settings.gitLazyFetch.get());
}

TEST_F(GitLazyFetchSettingTest, ExplicitConfigFalseOverridesEnvVarTrue)
{
    /* The key precedence guarantee: env seeds the *default* via
       `setDefault`, which is a no-op once a value has been explicitly
       `set` (which marks it `overridden`). So `nix.conf` /
       `--option git-lazy-fetch false` must win even with the env var
       set to "1". We exercise the real config path: a `Settings` whose
       env says "1", then an explicit `set(..., "false")`.

       NB the ordering mirrors reality: the env var is read in the
       constructor (before any config is loaded), and the explicit
       config assignment happens afterwards via `set`. */
    setEnv("NIX_GIT_LAZY_FETCH", "1");
    fetchers::Settings settings;
    /* Constructor already seeded the default to true. */
    ASSERT_TRUE(settings.gitLazyFetch.get());
    /* An explicit config value must override it. */
    ASSERT_TRUE(settings.set("git-lazy-fetch", "false"));
    EXPECT_FALSE(settings.gitLazyFetch.get());
}

TEST_F(GitLazyFetchSettingTest, SettingIsRegisteredUnderItsDocumentedName)
{
    /* The setting must be registered under the documented name so
       `nix.conf` / `--option git-lazy-fetch` actually reach it
       (`Config::set` returns false for an unknown name). */
    fetchers::Settings settings;
    EXPECT_TRUE(settings.set("git-lazy-fetch", "true"));
    /* A bogus name must NOT be silently accepted. */
    EXPECT_FALSE(settings.set("git-lazy-fetch-typo", "true"));
}

} // namespace nix
