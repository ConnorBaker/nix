#include "nix/util/string-intern-table.hh"

#include <cassert>

namespace nix {

/// Non-null empty string_view for out-of-range resolveRaw.
static constexpr std::string_view emptyView{""};

StringInternTable::StringInternTable()
    : dedup(0, IdxHash{store}, IdxEqual{store})
{
    store.add(std::string(""));  // sentinel at index 0 — NOT in dedup
}

uint32_t StringInternTable::internRaw(std::string_view sv)
{
    uint32_t result = 0;
    dedup.insert_and_cvisit(
        Key{store, sv, boost::hash<std::string_view>{}(sv)},
        [&](const uint32_t & v) { result = v; },   // inserted
        [&](const uint32_t & v) { result = v; });   // found
    return result;
}

uint32_t StringInternTable::findRaw(std::string_view sv) const
{
    uint32_t result = 0;
    dedup.cvisit(sv, [&](const uint32_t & idx) { result = idx; });
    return result;
}

std::string_view StringInternTable::resolveRaw(uint32_t idx) const
{
    if (idx >= store.size())
        return emptyView;
    return store[idx];
}

void StringInternTable::bulkLoad(uint32_t id, std::string_view sv)
{
    if (id < store.size())
        return;  // sentinel or already loaded
    while (store.size() < id)
        store.add(std::string());  // fill gaps
    auto [s, idx] = store.add(std::string(sv));
    assert(idx == id);
    dedup.insert(idx);
}

} // namespace nix
