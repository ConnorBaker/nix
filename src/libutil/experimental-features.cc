#include "nix/util/experimental-features.hh"
#include "nix/util/fmt.hh"
#include "nix/util/strings.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>

namespace nix {

struct ExperimentalFeatureDetails
{
    ExperimentalFeature tag;
    std::string_view name;
    std::string_view description;
    std::string_view trackingUrl;
};

/**
 * If two different PRs both add an experimental feature, and we just
 * used a number for this, we *woudln't* get merge conflict and the
 * counter will be incremented once instead of twice, causing a build
 * failure.
 *
 * By instead defining this instead as 1 + the bottom experimental
 * feature, we either have no issue at all if few features are not added
 * at the end of the list, or a proper merge conflict if they are.
 */
constexpr size_t numXpFeatures = 1 + static_cast<size_t>(Xp::ShardedLinks);

constexpr std::array<ExperimentalFeatureDetails, numXpFeatures> xpFeatureDetails = {{
    {
        .tag = Xp::CaDerivations,
        .name = "ca-derivations",
        .description = R"(
            Allow derivations to be content-addressed in order to prevent
            rebuilds when changes to the derivation do not result in changes to
            the derivation's output. See
            [__contentAddressed](@docroot@/language/advanced-attributes.md#adv-attr-__contentAddressed)
            for details.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/35",
    },
    {
        .tag = Xp::ImpureDerivations,
        .name = "impure-derivations",
        .description = R"(
            Allow derivations to produce non-fixed outputs by setting the
            `__impure` derivation attribute to `true`. An impure derivation can
            have differing outputs each time it is built.

            Example:

            ```
            derivation {
              name = "impure";
              builder = /bin/sh;
              __impure = true; # mark this derivation as impure
              args = [ "-c" "read -n 10 random < /dev/random; echo $random > $out" ];
              system = builtins.currentSystem;
            }
            ```

            Each time this derivation is built, it can produce a different
            output (as the builder outputs random bytes to `$out`).  Impure
            derivations also have access to the network, and only fixed-output
            or other impure derivations can rely on impure derivations. Finally,
            an impure derivation cannot also be
            [content-addressed](#xp-feature-ca-derivations).

            This is a more explicit alternative to using [`builtins.currentTime`](@docroot@/language/builtins.md#builtins-currentTime).
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/42",
    },
    {
        .tag = Xp::Flakes,
        .name = "flakes",
        .description = R"(
            Enable flakes. See the manual entry for [`nix
            flake`](@docroot@/command-ref/new-cli/nix3-flake.md) for details.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/27",
    },
    {
        .tag = Xp::FetchTree,
        .name = "fetch-tree",
        .description = R"(
            Enable the use of the [`fetchTree`](@docroot@/language/builtins.md#builtins-fetchTree) built-in function in the Nix language.

            `fetchTree` exposes a generic interface for fetching remote file system trees from different types of remote sources.
            The [`flakes`](#xp-feature-flakes) feature flag always enables `fetch-tree`.
            This built-in was previously guarded by the `flakes` experimental feature because of that overlap.

            Enabling just this feature serves as a "release candidate", allowing users to try it out in isolation.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/31",
    },
    {
        .tag = Xp::NixCommand,
        .name = "nix-command",
        .description = R"(
            Enable the new `nix` subcommands. See the manual on
            [`nix`](@docroot@/command-ref/new-cli/nix.md) for details.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/28",
    },
    {
        .tag = Xp::GitHashing,
        .name = "git-hashing",
        .description = R"(
            Allow creating (content-addressed) store objects which are hashed via Git's hashing algorithm.
            These store objects aren't understandable by older versions of Nix.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/41",
    },
    {
        .tag = Xp::RecursiveNix,
        .name = "recursive-nix",
        .description = R"(
            Allow derivation builders to call Nix, and thus build derivations
            recursively.

            Example:

            ```
            with import <nixpkgs> {};

            runCommand "foo"
              {
                 # Optional: let Nix know "foo" requires the experimental feature
                 requiredSystemFeatures = [ "recursive-nix" ];
                 buildInputs = [ nix jq ];
                 NIX_PATH = "nixpkgs=${<nixpkgs>}";
              }
              ''
                hello=$(nix-build -E '(import <nixpkgs> {}).hello.overrideDerivation (args: { name = "recursive-hello"; })')

                mkdir -p $out/bin
                ln -s $hello/bin/hello $out/bin/hello
              ''
            ```

            An important restriction on recursive builders is disallowing
            arbitrary substitutions. For example, running

            ```
            nix-store -r /nix/store/lrs9qfm60jcgsk83qhyypj3m4jqsgdid-hello-2.10
            ```

            in the above `runCommand` script would be disallowed, as this could
            lead to derivations with hidden dependencies or breaking
            reproducibility by relying on the current state of the Nix store. An
            exception would be if
            `/nix/store/lrs9qfm60jcgsk83qhyypj3m4jqsgdid-hello-2.10` were
            already in the build inputs or built by a previous recursive Nix
            call.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/47",
    },
    {
        .tag = Xp::FetchClosure,
        .name = "fetch-closure",
        .description = R"(
            Enable the use of the [`fetchClosure`](@docroot@/language/builtins.md#builtins-fetchClosure) built-in function in the Nix language.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/40",
    },
    {
        .tag = Xp::AutoAllocateUids,
        .name = "auto-allocate-uids",
        .description = R"(
            Allows Nix to automatically pick UIDs for builds, rather than creating
            `nixbld*` user accounts. See the [`auto-allocate-uids`](@docroot@/command-ref/conf-file.md#conf-auto-allocate-uids) setting for details.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/34",
    },
    {
        .tag = Xp::Cgroups,
        .name = "cgroups",
        .description = R"(
            Allows Nix to execute builds inside cgroups. See
            the [`use-cgroups`](@docroot@/command-ref/conf-file.md#conf-use-cgroups) setting for details.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/36",
    },
    {
        .tag = Xp::DaemonTrustOverride,
        .name = "daemon-trust-override",
        .description = R"(
            Allow forcing trusting or not trusting clients with
            `nix-daemon`. This is useful for testing, but possibly also
            useful for various experiments with `nix-daemon --stdio`
            networking.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/38",
    },
    {
        .tag = Xp::DynamicDerivations,
        .name = "dynamic-derivations",
        .description = R"(
            Allow the use of a few things related to dynamic derivations:

              - "text hashing" derivation outputs, so we can build .drv
                files.

              - dependencies in derivations on the outputs of
                derivations that are themselves derivations outputs.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/39",
    },
    {
        .tag = Xp::ParseTomlTimestamps,
        .name = "parse-toml-timestamps",
        .description = R"(
            Allow parsing of timestamps in builtins.fromTOML.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/45",
    },
    {
        .tag = Xp::ReadOnlyLocalStore,
        .name = "read-only-local-store",
        .description = R"(
            Allow the use of the `read-only` parameter in [local store](@docroot@/store/types/local-store.md) URIs.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/46",
    },
    {
        .tag = Xp::LocalOverlayStore,
        .name = "local-overlay-store",
        .description = R"(
            Allow the use of [local overlay store](@docroot@/command-ref/new-cli/nix3-help-stores.md#experimental-local-overlay-store).
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/50",
    },
    {
        .tag = Xp::ConfigurableImpureEnv,
        .name = "configurable-impure-env",
        .description = R"(
            Allow the use of the [impure-env](@docroot@/command-ref/conf-file.md#conf-impure-env) setting.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/37",
    },
    {
        .tag = Xp::MountedSSHStore,
        .name = "mounted-ssh-store",
        .description = R"(
            Allow the use of the [`mounted SSH store`](@docroot@/command-ref/new-cli/nix3-help-stores.html#experimental-ssh-store-with-filesystem-mounted).
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/43",
    },
    {
        .tag = Xp::VerifiedFetches,
        .name = "verified-fetches",
        .description = R"(
            Enables verification of git commit signatures through the [`fetchGit`](@docroot@/language/builtins.md#builtins-fetchGit) built-in.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/48",
    },
    {
        .tag = Xp::PipeOperators,
        .name = "pipe-operators",
        .description = R"(
            Add `|>` and `<|` operators to the Nix language.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/55",
    },
    {
        .tag = Xp::ExternalBuilders,
        .name = "external-builders",
        .description = R"(
            Enables support for external builders / sandbox providers.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/62",
    },
    {
        .tag = Xp::BLAKE3Hashes,
        .name = "blake3-hashes",
        .description = R"(
            Enables support for BLAKE3 hashes.
        )",
        .trackingUrl = "https://github.com/NixOS/nix/milestone/60",
    },
    {
        .tag = Xp::ShardedLinks,
        .name = "sharded-links",
        .description = R"(
            Change `/nix/store/.links/` from a single flat directory
            to a two-level sharded tree
            `/nix/store/.links/<pfx>/...`, where `<pfx>` is the
            first two Nix-base32 characters of the hash. Reduces
            kernel VFS rwsem contention on the shared directory
            under parallel `nix store optimise` and GC, and splits
            the htree of very large stores (tens of millions of
            entries) into 1024 smaller directories (pre-created at
            store open).

            The entry filename inside each shard is `<hash>` for
            the primary canonical and `<hash>.<NN>` (zero-padded
            two-digit decimal) for any replica past the primary,
            matching the flat layout's encoding. The replica /
            spillover scheme is governed by the `max-link-replicas`
            setting and is independent of this feature: a flat
            store can have replicas, and a sharded store can have
            `max-link-replicas=1` (no spillover), so the two axes
            can be combined or used separately.

            ## Migration

            When `nix store optimise` runs with this feature
            enabled on a store that still has legacy flat
            `.links/<hash>` entries, it performs a one-shot bulk
            migration up front — every flat entry is moved (via
            `link(2)` + `unlink(2)`) into its corresponding
            `<pfx>/<hash>` slot before any per-path dedup work
            begins. `link(2)` returns EEXIST without touching the
            destination if the primary slot is already taken (e.g.
            the flag was toggled off, new flat entries were
            created, then toggled on again); in that case the flat
            inode spills into the next free replica
            (`<pfx>/<hash>.01`, …) rather than being clobbered. We
            deliberately avoid `rename(2)` here, which would
            replace the destination atomically and orphan the
            user-store hardlinks pointing at it. Crash-recovery is
            handled by re-running migration: a stranded source +
            same-inode destination is detected and the source
            unlinked.

            ## Operational notes

            * Both layouts are walked unconditionally by GC and by
              `optimisePath_`'s inode-hash fast path, so mixed
              layouts (some hashes flat, others sharded) work
              correctly. This is what makes the flag safe to toggle
              on and off.
            * Disabling the flag after migration leaves sharded
              entries in place — they remain discoverable by every
              code path that walks `.links/` — but new canonical
              entries go flat. There is no reverse-migration tool;
              a fully flat store requires an empty `.links/` as a
              starting point.
            * The shard directory layer follows a strict
              `<2-char-prefix>/<hash>[.<NN>]` convention. Do not
              rename shard directories or their entries manually —
              parsers rely on the replica suffix (when present)
              being exactly two decimal digits.
        )",
        .trackingUrl = "",
    },
}};

static_assert(
    []() constexpr {
        for (auto [index, feature] : enumerate(xpFeatureDetails))
            if (index != (size_t) feature.tag)
                return false;
        return true;
    }(),
    "array order does not match enum tag order");

const std::optional<ExperimentalFeature> parseExperimentalFeature(const std::string_view & name)
{
    using ReverseXpMap = std::map<std::string_view, ExperimentalFeature>;

    static std::unique_ptr<ReverseXpMap> reverseXpMap = []() {
        auto reverseXpMap = std::make_unique<ReverseXpMap>();
        for (auto & xpFeature : xpFeatureDetails)
            (*reverseXpMap)[xpFeature.name] = xpFeature.tag;
        return reverseXpMap;
    }();

    if (auto feature = get(*reverseXpMap, name))
        return *feature;
    else
        return std::nullopt;
}

std::string_view showExperimentalFeature(const ExperimentalFeature tag)
{
    assert((size_t) tag < xpFeatureDetails.size());
    return xpFeatureDetails[(size_t) tag].name;
}

nlohmann::json documentExperimentalFeatures()
{
    StringMap res;
    for (auto & xpFeature : xpFeatureDetails) {
        std::stringstream docOss;
        docOss << stripIndentation(xpFeature.description);
        /* Only emit the tracking-issue footer when a URL is present.
           An empty `trackingUrl` would render as the broken markdown
           link `[<name> tracking issue]()` — better to omit the line
           entirely than to ship a dead link in the generated manual.
           Use this for features whose tracking infrastructure
           doesn't exist yet (e.g. in-development branches). */
        if (!xpFeature.trackingUrl.empty()) {
            docOss << fmt(
                "\nRefer to [%1% tracking issue](%2%) for feature tracking.",
                xpFeature.name,
                xpFeature.trackingUrl);
        }
        res[std::string{xpFeature.name}] = trim(docOss.str());
    }
    return (nlohmann::json) res;
}

std::set<ExperimentalFeature> parseFeatures(const StringSet & rawFeatures)
{
    std::set<ExperimentalFeature> res;
    for (auto & rawFeature : rawFeatures)
        if (auto feature = parseExperimentalFeature(rawFeature))
            res.insert(*feature);
    return res;
}

MissingExperimentalFeature::MissingExperimentalFeature(ExperimentalFeature feature, std::string reason)
    : CloneableError(
          "experimental Nix feature '%1%' is disabled%2%; add '--extra-experimental-features %1%' to enable it",
          showExperimentalFeature(feature),
          Uncolored(optionalBracket(" (", reason, ")")))
    , missingFeature(feature)
    , reason{reason}
{
}

std::ostream & operator<<(std::ostream & str, const ExperimentalFeature & feature)
{
    return str << showExperimentalFeature(feature);
}

void to_json(nlohmann::json & j, const ExperimentalFeature & feature)
{
    j = showExperimentalFeature(feature);
}

void from_json(const nlohmann::json & j, ExperimentalFeature & feature)
{
    const std::string input = j;
    const auto parsed = parseExperimentalFeature(input);

    if (parsed.has_value())
        feature = *parsed;
    else
        throw Error("Unknown experimental feature '%s' in JSON input", input);
}

} // namespace nix
