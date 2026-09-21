#pragma once
///@file

#include <nlohmann/json.hpp>

#include "nix/store/path-info.hh"
#include "nix/util/tests/characterization.hh"

namespace nix {

/* JSON goldens of path infos read from OLD wire forms: such an info carries
   the NAR hash the peer sent (`assertedNarHash`) and no object hash, so the
   nlohmann serializer's default, format 4, cannot render it; the goldens are
   format 3 and are written as such here, byte for byte as before.  Shared by
   the worker and serve protocol tests (one translation unit under the unity
   build). */
inline nlohmann::json pathInfoJsonV3(const UnkeyedValidPathInfo & info)
{
    return info.toJSON(nullptr, true, PathInfoJsonFormat::V3);
}

inline nlohmann::json pathInfoJsonV3(const ValidPathInfo & info)
{
    auto j = info.toJSON(nullptr, true, PathInfoJsonFormat::V3);
    j["path"] = info.path;
    return j;
}

template<typename... Ts>
nlohmann::json pathInfosJsonV3(const std::tuple<Ts...> & infos)
{
    auto arr = nlohmann::json::array();
    std::apply([&](const auto &... info) { (arr.push_back(pathInfoJsonV3(info)), ...); }, infos);
    return arr;
}

} // namespace nix

#define VERSIONED_CHARACTERIZATION_TEST_JSON_V3(FIXTURE, NAME, STEM, VERSION, VALUE)                  \
    VERSIONED_READ_CHARACTERIZATION_TEST(FIXTURE, NAME, STEM, (VERSION), VALUE)                       \
    VERSIONED_WRITE_CHARACTERIZATION_TEST_NO_JSON(FIXTURE, NAME, STEM, (VERSION), VALUE)              \
    TEST_F(FIXTURE, NAME##_json_write)                                                                \
    {                                                                                                 \
        writeTest(                                                                                    \
            std::string{STEM} + ".json",                                                              \
            [&]() -> nlohmann::json { return pathInfosJsonV3(VALUE); },                               \
            [](const auto & file) { return nlohmann::json::parse(readFile(file)); },                  \
            [](const auto & file, const auto & got) { return writeFile(file, got.dump(2) + "\n"); }); \
    }
