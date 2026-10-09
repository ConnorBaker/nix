#pragma once
///@file

#include "nix/util/error.hh"
#include <functional>
#include <map>
#include <ranges>
#include <variant>
#include <concepts>

namespace nix {

template<typename T>
struct Cycle
{
    T path;
    T parent;
};

template<typename T>
using TopoSortResult = std::variant<std::vector<T>, Cycle<T>>;

/** Emit each completed DFS visit. Callbacks must leave items unchanged;
 * return the first cycle edge without undoing the already emitted prefix. */
template<typename Items, typename T = typename Items::key_type, typename Compare = typename Items::key_compare,
         std::invocable<const T &> F, typename Emit>
    requires std::same_as<std::remove_cvref_t<std::invoke_result_t<F, const T &>>, std::set<T, Compare>>
    && std::invocable<const Emit &, const T &>
std::optional<Cycle<T>> visitDependencies(const Items & items, const F & getChildren, const Emit & emit)
{
    Compare compare;
    std::map<T, bool, std::reference_wrapper<Compare>> visited{std::ref(compare)};

    auto dfsVisit = [&](this const auto & self, const T & path, const T * parent) -> std::optional<Cycle<T>> {
        auto [visit, first] = visited.try_emplace(path, true);
        if (!first)
            return visit->second ? std::optional{Cycle{path, *parent}} : std::nullopt;

        auto && references = std::invoke(getChildren, path);

        for (auto & i : references)
            /* Don't traverse into items that don't exist in our starting set. */
            if (i != path && items.count(i)) {
                if (auto cycle = self(i, &path))
                    return cycle;
            }

        std::invoke(emit, path);
        visit->second = false;

        return std::nullopt;
    };

    for (auto & i : items | std::views::transform([](const auto & item) -> const T & {
             if constexpr (std::same_as<typename Items::value_type, T>) return item;
             else return item.first;
         }))
        if (auto cycle = dfsVisit(i, nullptr))
            return cycle;
    return std::nullopt;
}

template<typename T, typename Compare, std::invocable<const T &> F>
    requires std::same_as<std::remove_cvref_t<std::invoke_result_t<F, const T &>>, std::set<T, Compare>>
TopoSortResult<T> topoSort(std::set<T, Compare> items, const F & getChildren)
{
    std::vector<T> sorted;
    if (auto cycle = visitDependencies(items, getChildren, [&](const T & path) { sorted.push_back(path); }))
        return *cycle;
    std::reverse(sorted.begin(), sorted.end());

    return sorted;
}

} // namespace nix
