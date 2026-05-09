#pragma once
///@file
/// Binary framing primitives shared between trace-serialize.cc and
/// input-resolution.cc.  Private internal header (not installed).
///
/// Each blob prefix ("dsp1", "spa1", "rfi1", "dsrc2", etc.) is independently
/// versioned; these helpers only handle the shared framing (uint64-length-
/// prefixed strings, fetcher-attribute encoding).

#include "nix/fetchers/fetchers.hh"
#include "nix/util/util.hh"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nix::eval_trace {

inline void appendUint64(std::string & out, uint64_t value)
{
    for (size_t i = 0; i < sizeof(value); ++i)
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

inline std::optional<uint64_t> readUint64(std::string_view in, size_t & offset)
{
    if (offset + sizeof(uint64_t) > in.size())
        return std::nullopt;

    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i)
        value |= static_cast<uint64_t>(static_cast<unsigned char>(in[offset + i])) << (i * 8);
    offset += sizeof(uint64_t);
    return value;
}

inline std::optional<std::string> readFramedString(std::string_view in, size_t & offset)
{
    auto len = readUint64(in, offset);
    if (!len || offset + *len > in.size())
        return std::nullopt;
    auto value = std::string(in.substr(offset, *len));
    offset += *len;
    return value;
}

inline void appendFetcherAttr(std::string & out, const fetchers::Attr & attr)
{
    std::visit(
        overloaded{
            [&](const std::string & value) {
                out.push_back('s');
                appendUint64(out, value.size());
                out.append(value);
            },
            [&](uint64_t value) {
                out.push_back('u');
                appendUint64(out, value);
            },
            [&](Explicit<bool> value) {
                out.push_back('b');
                out.push_back(value.t ? '\x01' : '\x00');
            },
            [&](const fetchers::LazyAttr &) {
                // Encode only the presence of a lazy attr, not its value;
                // lazy and concrete attrs produce distinct blobs by design
                // to avoid forcing the computation during cache lookup.
                out.push_back('z');
            },
        },
        attr);
}

inline std::optional<fetchers::Attr> readFetcherAttr(std::string_view in, size_t & offset)
{
    if (offset >= in.size())
        return std::nullopt;

    auto tag = in[offset++];
    switch (tag) {
    case 's': {
        auto value = readFramedString(in, offset);
        if (!value)
            return std::nullopt;
        return fetchers::Attr{std::move(*value)};
    }
    case 'u': {
        auto value = readUint64(in, offset);
        if (!value)
            return std::nullopt;
        return fetchers::Attr{*value};
    }
    case 'b': {
        if (offset >= in.size())
            return std::nullopt;
        return fetchers::Attr{Explicit<bool>{in[offset++] != '\x00'}};
    }
    case 'z':
        // Lazy attrs are encoded as presence-only markers. Decoding
        // rebuilds a placeholder LazyAttr whose compute() throws if
        // ever invoked — this path is for dep-key reconstruction
        // (cache lookup / display), not for evaluation. Any attempt
        // to force the value indicates a logic error, since the
        // originating fetcher is not available on the verify side.
        return fetchers::Attr{fetchers::LazyAttr{
            make_ref<fetchers::LazyAttrComputation>(fetchers::LazyAttrComputation{
                .compute = []() -> fetchers::ResolvedAttr {
                    throw Error("internal error: decoded lazy fetcher attribute cannot be forced outside its originating fetcher");
                },
            })}};
    default:
        return std::nullopt;
    }
}

} // namespace nix::eval_trace
