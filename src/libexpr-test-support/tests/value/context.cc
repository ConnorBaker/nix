#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <rapidcheck.h>

#include "nix/expr/tests/value/context.hh"
#include "nix/store/source-content-id.hh"
#include "nix/util/hash.hh"

namespace rc {
using namespace nix;

Gen<NixStringContextElem::DrvDeep> Arbitrary<NixStringContextElem::DrvDeep>::arbitrary()
{
    return gen::map(gen::arbitrary<StorePath>(), [](StorePath drvPath) {
        return NixStringContextElem::DrvDeep{
            .drvPath = drvPath,
        };
    });
}

Gen<NixStringContextElem::SourceVirtual> Arbitrary<NixStringContextElem::SourceVirtual>::arbitrary()
{
    /* Generate a deterministic placeholder from a random fingerprint
       string. Names are simple ASCII to satisfy the parser's
       no-space-in-name and no-empty-name constraints. */
    return gen::mapcat(gen::nonEmpty(gen::string<std::string>()), [](std::string fingerprint) {
        return gen::map(
            gen::nonEmpty(
                gen::container<std::string>(gen::elementOf(std::string("abcdefghijklmnopqrstuvwxyz0123456789")))),
            [fingerprint = std::move(fingerprint)](std::string name) {
                auto cid = SourceContentId::compute(
                    fingerprint, Hash(HashAlgorithm::SHA256), ContentAddressMethod::Raw::NixArchive, StoreReferences{});
                return NixStringContextElem::SourceVirtual{
                    .placeholder = SourcePlaceholder::make(cid, name),
                    .name = name,
                };
            });
    });
}

Gen<NixStringContextElem> Arbitrary<NixStringContextElem>::arbitrary()
{
    return gen::mapcat(
        gen::inRange<uint8_t>(0, std::variant_size_v<NixStringContextElem::Raw>),
        [](uint8_t n) -> Gen<NixStringContextElem> {
            switch (n) {
            case 0:
                return gen::map(
                    gen::arbitrary<NixStringContextElem::Opaque>(), [](NixStringContextElem a) { return a; });
            case 1:
                return gen::map(
                    gen::arbitrary<NixStringContextElem::DrvDeep>(), [](NixStringContextElem a) { return a; });
            case 2:
                return gen::map(
                    gen::arbitrary<NixStringContextElem::Built>(), [](NixStringContextElem a) { return a; });
            case 3:
                return gen::map(
                    gen::arbitrary<NixStringContextElem::SourceVirtual>(), [](NixStringContextElem a) { return a; });
            default:
                assert(false);
            }
        });
}

} // namespace rc
