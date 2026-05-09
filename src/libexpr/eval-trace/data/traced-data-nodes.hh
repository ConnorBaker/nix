#pragma once
///@file
/// Concrete TracedDataNode payload definitions.
///
/// Private header.  Only included by:
///   - the three parsers that construct nodes (`json-to-value.cc`,
///     `primops/fromTOML.cc`, `primops.cc` for readDir).
///   - the dispatcher `traced-data-dispatch.cc` that implements
///     `TracedDataNode::objectKeys` etc.
///
/// Not installed; not meant for wider consumption.  Pulls in
/// `<nlohmann/json.hpp>` and `<toml.hpp>`, which is expensive at
/// include time — every file transitively including this forces
/// those two heavy headers.  Keeping this out of the installed
/// public header avoids the include-bloat everywhere else.
///
/// No virtuals.  Every concrete class's ctor initialises the base
/// `TracedDataNode::tag_` to the right `Tag` variant; the dispatcher
/// switches on that tag and `static_cast`s to the concrete type.

#include "nix/expr/eval-trace/data/traced-data.hh"
#include "nix/util/source-accessor.hh"

#include "expr-config-private.hh"

#include <nlohmann/json.hpp>
#include <toml.hpp>

#include <optional>

namespace nix {

using nlohmann::json;

#if HAVE_TOML11_4
/// Subsecond-precision rounding used by `normalizeDatetimeFormat`.
/// Inlined here so the dispatcher TU and `fromTOML.cc` share a
/// single definition without duplicating the function in both.
inline size_t normalizeSubsecondPrecisionTomlNode(toml::local_time lt)
{
    auto millis = lt.millisecond;
    auto micros = lt.microsecond;
    auto nanos = lt.nanosecond;
    if (millis != 0 || micros != 0 || nanos != 0) {
        if (micros != 0 || nanos != 0) {
            if (nanos != 0) return 9;
            return 6;
        }
        return 3;
    }
    return 0;
}

/// TOML11 pre-4.0 datetime-format normalization.  Used at both the
/// traced scalar materialization path (this file's dispatcher) and
/// the non-traced `prim_fromTOML` raw-parse path (`fromTOML.cc`).
/// Kept inline so both TUs see the same definition and the linker
/// doesn't care which one wins.
inline void normalizeDatetimeFormatTomlNode(toml::value & t)
{
    if (t.is_local_datetime()) {
        auto & ldt = t.as_local_datetime();
        t.as_local_datetime_fmt() = {
            .delimiter = toml::datetime_delimiter_kind::upper_T,
            .has_seconds = true,
            .subsecond_precision = normalizeSubsecondPrecisionTomlNode(ldt.time),
        };
        return;
    }
    if (t.is_offset_datetime()) {
        auto & odt = t.as_offset_datetime();
        t.as_offset_datetime_fmt() = {
            .delimiter = toml::datetime_delimiter_kind::upper_T,
            .has_seconds = true,
            .subsecond_precision = normalizeSubsecondPrecisionTomlNode(odt.time),
        };
        return;
    }
    if (t.is_local_time()) {
        auto & lt = t.as_local_time();
        t.as_local_time_fmt() = {
            .has_seconds = true,
            .subsecond_precision = normalizeSubsecondPrecisionTomlNode(lt),
        };
        return;
    }
}
#endif

/// JSON-backed node.  Root node owns the full DOM; child nodes hold
/// a non-owning pointer into the root's DOM.  Both carry a GC
/// pointer to the root so the root stays alive while any child is
/// reachable.
struct JsonDataNode : TracedDataNode {
    json ownedData;       ///< meaningful only on the root
    const json * data;
    JsonDataNode * root;  ///< GC pointer keeps root alive for children

    /// Root-node ctor: takes ownership of a parsed DOM.
    explicit JsonDataNode(json d)
        : TracedDataNode(tagFor(d))
        , ownedData(std::move(d))
        , data(&ownedData)
        , root(this)
    {}

    /// Child-node ctor: zero-copy pointer into the parent's DOM.
    JsonDataNode(const json * d, JsonDataNode * r)
        : TracedDataNode(tagFor(*d))
        , data(d)
        , root(r)
    {}

private:
    static Tag tagFor(const json & j) noexcept
    {
        switch (j.type()) {
        case json::value_t::object: return Tag::JsonObject;
        case json::value_t::array:  return Tag::JsonArray;
        case json::value_t::string: return Tag::JsonString;
        case json::value_t::number_integer:
        case json::value_t::number_unsigned:
        case json::value_t::number_float: return Tag::JsonNumber;
        case json::value_t::boolean: return Tag::JsonBool;
        case json::value_t::null:
        case json::value_t::discarded:
        case json::value_t::binary: return Tag::JsonNull;
        }
        return Tag::JsonNull;
    }
};

/// TOML-backed node.  Same root/child ownership as JSON.
struct TomlDataNode : TracedDataNode {
    toml::value ownedData;       ///< meaningful only on the root
    const toml::value * data;
    TomlDataNode * root;

    /// Root-node ctor.
    explicit TomlDataNode(toml::value d)
        : TracedDataNode(tagFor(d))
        , ownedData(std::move(d))
        , data(&ownedData)
        , root(this)
    {}

    /// Child-node ctor.
    TomlDataNode(const toml::value * d, TomlDataNode * r)
        : TracedDataNode(tagFor(*d))
        , data(d)
        , root(r)
    {}

private:
    static Tag tagFor(const toml::value & v) noexcept
    {
        switch (v.type()) {
        case toml::value_t::table:   return Tag::TomlObject;
        case toml::value_t::array:   return Tag::TomlArray;
        case toml::value_t::string:  return Tag::TomlString;
        case toml::value_t::integer:
        case toml::value_t::floating: return Tag::TomlNumber;
        case toml::value_t::boolean:  return Tag::TomlBool;
        case toml::value_t::empty:    return Tag::TomlNull;
        /// Datetime types are materialised as strings (see the
        /// datetime branch of the dispatcher's `materializeScalar`).
        case toml::value_t::offset_datetime:
        case toml::value_t::local_datetime:
        case toml::value_t::local_date:
        case toml::value_t::local_time: return Tag::TomlString;
        }
        return Tag::TomlNull;
    }
};

/// Directory-listing root node.  Keys are entry names; values are
/// `DirScalarNode` leaves.  Object-kind only — readDir output is
/// always flat.
struct DirDataNode : TracedDataNode {
    SourceAccessor::DirEntries entries;

    explicit DirDataNode(SourceAccessor::DirEntries entries)
        : TracedDataNode(Tag::DirObject)
        , entries(std::move(entries))
    {}
};

/// Scalar leaf for a single directory entry's type.
struct DirScalarNode : TracedDataNode {
    std::optional<SourceAccessor::Type> entryType;

    explicit DirScalarNode(std::optional<SourceAccessor::Type> type)
        : TracedDataNode(Tag::DirScalar)
        , entryType(type)
    {}
};

} // namespace nix
