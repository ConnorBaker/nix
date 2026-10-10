#pragma once

#include "nix/fetchers/fetchers.hh"

namespace nix::fetchers {

enum class UseRegistries : int;
struct Settings;

struct InputCache
{
    struct CachedResult
    {
        FixedSource accessor;
        Input resolvedInput;
        Input lockedInput;
        Attrs extraAttrs;
    };

    CachedResult
    getAccessor(const Settings & settings, Store & store, const Input & originalInput, UseRegistries useRegistries);

    struct CachedInput
    {
        Input lockedInput;
        FixedSource accessor;
        Attrs extraAttrs;
    };

    virtual std::optional<CachedInput> lookup(const Input & originalInput) const = 0;

    /** The caller supplies the producer-owned fixed value promised by FixedSource. */
    virtual void upsert(Input key, CachedInput cachedInput) = 0;

    virtual void clear() = 0;

    static ref<InputCache> create();

    virtual ~InputCache() = default;
};

} // namespace nix::fetchers
