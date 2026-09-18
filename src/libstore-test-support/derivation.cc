#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <rapidcheck.h>

#include "nix/store/tests/derivation.hh"

namespace rc {
using namespace nix;

Gen<derivation::Output> Arbitrary<derivation::Output>::arbitrary()
{
    return gen::map(gen::arbitrary<StorePath>(), [](StorePath path) {
        return derivation::Output{derivation::Output::InputAddressed{.path = std::move(path)}};
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
           std::set<SingleDerivedPath::Opaque> inputSrcs,
           derivation::Outputs<derivation::Output> outputs) {
            Derivation drv;
            drv.name = std::move(name.name);
            drv.platform = std::move(platform);
            drv.builder = std::move(builder);
            drv.args = std::move(args);
            /* The ATerm serialisation reserves `__json` for structured
               attributes; the environment must not contain it. */
            env.erase("__json");
            drv.env = std::move(env);
            for (auto & src : inputSrcs)
                drv.inputs.insert(SingleDerivedPath{src});
            drv.outputs = std::move(outputs);
            return drv;
        },
        gen::arbitrary<StorePathName>(),
        /* The platform is written verbatim, so the format cannot hold a
           double quote or a backslash in it; the writer refuses them. */
        gen::map(
            gen::arbitrary<std::string>(),
            [](std::string platform) {
                std::erase_if(platform, [](char c) { return c == '"' || c == '\\'; });
                return platform;
            }),
        gen::arbitrary<std::string>(),
        gen::arbitrary<Strings>(),
        gen::arbitrary<StringPairs>(),
        gen::arbitrary<std::set<SingleDerivedPath::Opaque>>(),
        gen::nonEmpty(
            gen::container<derivation::Outputs<derivation::Output>>(
                gen::map(gen::arbitrary<StorePathName>(), [](StorePathName n) { return n.name; }),
                gen::arbitrary<derivation::Output>())));
}

} // namespace rc
