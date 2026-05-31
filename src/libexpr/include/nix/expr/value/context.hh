#pragma once
///@file

#include "nix/util/comparator.hh"
#include "nix/store/derived-path.hh"
#include "nix/store/source-placeholder.hh"
#include "nix/util/variant-wrapper.hh"

#include <nlohmann/json_fwd.hpp>

namespace nix {

class BadNixStringContextElem final : public CloneableError<BadNixStringContextElem, Error>
{
public:
    std::string_view raw;

    template<typename... Args>
    BadNixStringContextElem(std::string_view raw_, const Args &... args)
        : CloneableError("")
    {
        raw = raw_;
        auto hf = HintFmt(args...);
        err.msg = HintFmt("Bad String Context element: %1%: %2%", Uncolored(hf.str()), raw);
    }
};

/**
 * @todo This should be renamed to `StringContextBuilderElem`, since:
 *
 * 1. We use `*Builder` for off-heap temporary data structures
 *
 * 2. The `Nix*` is totally redundant. (And my mistake from a long time
 * ago.)
 */
struct NixStringContextElem
{
    /**
     * Plain opaque path to some store object.
     *
     * Encoded as just the path: `<path>`.
     */
    using Opaque = SingleDerivedPath::Opaque;

    /**
     * Path to a derivation and its entire build closure.
     *
     * The path doesn't just refer to derivation itself and its closure, but
     * also all outputs of all derivations in that closure (including the
     * root derivation).
     *
     * Encoded in the form `=<drvPath>`.
     */
    struct DrvDeep
    {
        StorePath drvPath;

        GENERATE_CMP(DrvDeep, me->drvPath);
    };

    /**
     * Derivation output.
     *
     * Encoded in the form `!<output>!<drvPath>`.
     */
    using Built = SingleDerivedPath::Built;

    /**
     * Source view that has not yet been materialised to a real
     * store path. Carries an opaque placeholder string identifying
     * the registered view in `MaterialisationScheduler` plus a
     * per-output `name` for disambiguation when several outputs
     * share one `SourceContentId` (the cargo-workspace pattern).
     *
     * Encoded in the form `~<base32-hash>:<name>` where the hash is
     * the placeholder's render() body (52 base32 chars). The leading
     * '~' is the sigil; ':' separates the hash from the name.
     *
     * On context realisation (`realiseContext`), the placeholder is
     * resolved to a real `StorePath` via the eval state's
     * `MaterialisationScheduler`. See `doc/tecnix-survey/PROPOSAL.md`
     * §"O' SourcePlaceholder + MaterialisationScheduler" for the
     * design.
     */
    struct SourceVirtual
    {
        SourcePlaceholder placeholder;
        std::string name;

        GENERATE_CMP(SourceVirtual, me->placeholder, me->name);
    };

    using Raw = std::variant<Opaque, DrvDeep, Built, SourceVirtual>;

    Raw raw;

    GENERATE_CMP(NixStringContextElem, me->raw);

    MAKE_WRAPPER_CONSTRUCTOR(NixStringContextElem);

    /**
     * Decode a context string, one of:
     * - `<path>`
     * - `=<path>`
     * - `!<name>!<path>`
     * - `~<base32-hash>:<name>` (SourceVirtual)
     *
     * @param xpSettings Stop-gap to avoid globals during unit tests.
     */
    static NixStringContextElem
    parse(std::string_view s, const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);
    std::string to_string() const;

    /**
     * Render for use in error messages and other user-facing output.
     *
     * Uses store paths and `DerivedPath` syntax, unlike `to_string()`
     * which uses the internal encoding.
     */
    std::string display(const StoreDirConfig & store) const;
};

/**
 * @todo This should be renamed to `StringContextBuilder`.
 *
 * @see NixStringContextElem for explanation why.
 */
typedef std::set<NixStringContextElem> NixStringContext;

} // namespace nix
