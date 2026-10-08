#pragma once
///@file

#include "nix/util/error.hh"
#include <functional>
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
template<typename T, typename Compare, std::invocable<const T &> F, typename Emit>
    requires std::same_as<std::remove_cvref_t<std::invoke_result_t<F, const T &>>, std::set<T, Compare>>
    && std::invocable<const Emit &, const T &>
std::optional<Cycle<T>> visitDependencies(const std::set<T, Compare> & items, const F & getChildren, const Emit & emit)
{
    std::set<T, Compare> visited, parents;

    auto dfsVisit = [&](this const auto & self, const T & path, const T * parent) -> std::optional<Cycle<T>> {
        if (parents.count(path)) {
            return Cycle{path, *parent};
        }

        if (!visited.insert(path).second) {
            return std::nullopt;
        }
        parents.insert(path);

        auto && references = std::invoke(getChildren, path);

        for (auto & i : references)
            /* Don't traverse into items that don't exist in our starting set. */
            if (i != path && items.count(i)) {
                if (auto cycle = self(i, &path))
                    return cycle;
            }

        std::invoke(emit, path);
        parents.erase(path);

        return std::nullopt;
    };

    for (auto & i : items)
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
