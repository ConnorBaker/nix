#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <rapidcheck.h>

#include "nix/store/tests/derivation.hh"

namespace rc {
using namespace nix;

Gen<DerivationOutput> Arbitrary<DerivationOutput>::arbitrary()
{
    return gen::map(gen::arbitrary<StorePath>(), [](StorePath path) {
        return DerivationOutput{
            DerivationOutput::InputAddressed{
                .path = std::move(path),
            }};
    });
}

Gen<Derivation> Arbitrary<Derivation>::arbitrary()
{
    return gen::apply(
        [](StorePathName name,
           std::string platform,
           std::string builder,
           Strings args,
           StringPairs env,
           StorePathSet inputSrcs,
           DerivationOutputs outputs) {
            Derivation drv;
            drv.name = std::move(name.name);
            drv.platform = std::move(platform);
            drv.builder = std::move(builder);
            drv.args = std::move(args);
            /* The ATerm serialization reserves `"__json"` for structured
               attrs; `BasicDerivation::env` must not contain it. */
            env.erase("__json");
            drv.env = std::move(env);
            drv.inputSrcs = std::move(inputSrcs);
            drv.outputs = std::move(outputs);
            /* `inputDrvs` is left empty (valid for `unparse`) and
               `structuredAttrs` `nullopt`. */
            return drv;
        },
        gen::arbitrary<StorePathName>(),
        gen::arbitrary<std::string>(),
        gen::arbitrary<std::string>(),
        gen::arbitrary<Strings>(),
        gen::arbitrary<StringPairs>(),
        gen::arbitrary<StorePathSet>(),
        gen::nonEmpty<DerivationOutputs>(gen::container<DerivationOutputs>(
            gen::map(gen::arbitrary<StorePathName>(), [](StorePathName n) { return n.name; }),
            gen::arbitrary<DerivationOutput>())));
}

} // namespace rc
