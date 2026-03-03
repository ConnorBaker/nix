#include "nix/util/string-intern-table.hh"

namespace nix {

/// Non-null empty string_view. Returned for empty strings, sentinels, gaps,
/// and out-of-range lookups so that callers never receive a null data pointer.
static constexpr std::string_view emptyView{""};

StringInternTable::StringInternTable()
    : dedup(0, ViewHash{&strings}, ViewEqual{&strings})
{
    strings.push_back(emptyView); // sentinel at index 0
}

std::string_view StringInternTable::copyToArena(std::string_view sv)
{
    if (sv.empty())
        return emptyView;
    char * data = static_cast<char *>(arena.allocate(sv.size(), 1));
    std::memcpy(data, sv.data(), sv.size());
    return {data, sv.size()};
}

uint32_t StringInternTable::internRaw(std::string_view sv)
{
    auto it = dedup.find(sv);
    if (it != dedup.end())
        return *it;
    uint32_t idx = static_cast<uint32_t>(strings.size());
    strings.push_back(copyToArena(sv));
    dedup.insert(idx);
    return idx;
}

uint32_t StringInternTable::findRaw(std::string_view sv) const
{
    auto it = dedup.find(sv);
    if (it != dedup.end())
        return *it;
    return 0;
}

std::string_view StringInternTable::resolveRaw(uint32_t idx) const
{
    if (idx < strings.size())
        return strings[idx];
    return emptyView;
}

void StringInternTable::bulkLoad(uint32_t id, std::string_view sv)
{
    if (id >= strings.size())
        strings.resize(id + 1, emptyView);
    strings[id] = copyToArena(sv);
    dedup.insert(id);
}

void StringInternTable::clear()
{
    dedup.clear();
    strings.clear();
    arena.release();
    strings.push_back(emptyView); // re-add sentinel
}

} // namespace nix
