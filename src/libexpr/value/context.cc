#include "nix/util/util.hh"
#include "nix/expr/value/context.hh"
#include "nix/store/store-dir-config.hh"

namespace nix {

void BadNixStringContextElem::anchor() {}

NixStringContextElem NixStringContextElem::parse(std::string_view s0, const ExperimentalFeatureSettings & xpSettings)
{
    std::string_view s = s0;

    auto parseRest = [&](this auto & parseRest) -> SingleDerivedPath {
        // Case on whether there is a '!'
        size_t index = s.find("!");
        if (index == std::string_view::npos) {
            return SingleDerivedPath::Opaque{
                .path = StorePath{s},
            };
        } else {
            std::string output{s.substr(0, index)};
            // Advance string to parse after the '!'
            s = s.substr(index + 1);
            auto drv = make_ref<SingleDerivedPath>(parseRest());
            drvRequireExperiment(*drv, xpSettings);
            return SingleDerivedPath::Built{
                .drvPath = std::move(drv),
                .output = std::move(output),
            };
        }
    };

    if (s.size() == 0) {
        throw BadNixStringContextElem(s0, "String context element should never be an empty string");
    }

    switch (s.at(0)) {
    case '!': {
        // Advance string to parse after the '!'
        s = s.substr(1);

        // Find *second* '!'
        if (s.find("!") == std::string_view::npos) {
            throw BadNixStringContextElem(s0, "String content element beginning with '!' should have a second '!'");
        }

        return std::visit([&](auto x) -> NixStringContextElem { return std::move(x); }, parseRest());
    }
    case '=': {
        return NixStringContextElem::DrvDeep{
            .drvPath = StorePath{s.substr(1)},
        };
    }
    default: {
        // Ensure no '!'
        if (s.find("!") != std::string_view::npos) {
            throw BadNixStringContextElem(
                s0, "String content element not beginning with '!' should not have a second '!'");
        }
        return std::visit([&](auto x) -> NixStringContextElem { return std::move(x); }, parseRest());
    }
    }
}

std::string NixStringContextElem::to_string() const
{
    std::string res;

    auto toStringRest = [&](this auto & toStringRest, const auto & path) -> void {
        std::visit(
            overloaded{
                [&](const Opaque & o) { res += o.path.to_string(); },
                [&](const Built & b) {
                    res += b.output;
                    res += '!';
                    toStringRest(b.drvPath->raw());
                },
                [&](const DrvDeep & d) {
                    res += '=';
                    res += d.drvPath.to_string();
                },
            },
            path);
    };

    if (std::holds_alternative<Built>(raw))
        res += '!';
    toStringRest(raw);

    return res;
}

std::string NixStringContextElem::display(const StoreDirConfig & store) const
{
    return std::visit(
        overloaded{
            [&](const auto & path) -> std::string { return path.to_string(store); },
            [&](const NixStringContextElem::DrvDeep & d) -> std::string {
                return store.printStorePath(d.drvPath) + " (deep)";
            },
        },
        raw);
}

} // namespace nix
