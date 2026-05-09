/// Dispatcher for `TracedDataNode` payload access.
///
/// `TracedDataNode` is devirtualised: the base carries a `Tag`
/// byte and the 6 payload-access member functions
/// (`objectKeys`, `objectGet`, `arraySize`, `arrayGet`,
/// `materializeScalar`, `canonicalValue`) are implemented here as
/// switches on `tag_` that `static_cast` to the matching concrete
/// type.  Zero vtable, zero vptr.
///
/// Every switch is exhaustive (`-Wswitch-enum` is `-Werror`).

#include "traced-data-nodes.hh"

#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/static-string-data.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "../toml-canonical.hh"

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nix {

// ── objectKeys ──────────────────────────────────────────────────

std::vector<std::string> TracedDataNode::objectKeys() const
{
    switch (tag_) {
    case Tag::JsonObject: {
        auto * self = static_cast<const JsonDataNode *>(this);
        std::vector<std::string> keys;
        keys.reserve(self->data->size());
        for (auto & [k, _] : self->data->items())
            keys.push_back(k);
        return keys;
    }
    case Tag::TomlObject: {
        auto * self = static_cast<const TomlDataNode *>(this);
        auto & table = toml::get<toml::table>(*self->data);
        std::vector<std::string> keys;
        keys.reserve(table.size());
        for (auto & [k, _] : table)
            keys.push_back(k);
        return keys;
    }
    case Tag::DirObject: {
        auto * self = static_cast<const DirDataNode *>(this);
        std::vector<std::string> keys;
        keys.reserve(self->entries.size());
        for (auto & [name, _] : self->entries)
            keys.push_back(name);
        return keys;
    }
    case Tag::JsonArray:
    case Tag::JsonString:
    case Tag::JsonNumber:
    case Tag::JsonBool:
    case Tag::JsonNull:
    case Tag::TomlArray:
    case Tag::TomlString:
    case Tag::TomlNumber:
    case Tag::TomlBool:
    case Tag::TomlNull:
    case Tag::DirScalar:
        return {};
    }
    return {};
}

// ── objectGet ───────────────────────────────────────────────────

TracedDataNode * TracedDataNode::objectGet(const std::string & key) const
{
    switch (tag_) {
    case Tag::JsonObject: {
        auto * self = static_cast<const JsonDataNode *>(this);
        return new JsonDataNode(&self->data->at(key), self->root);
    }
    case Tag::TomlObject: {
        auto * self = static_cast<const TomlDataNode *>(this);
        return new TomlDataNode(
            &toml::get<toml::table>(*self->data).at(key),
            self->root);
    }
    case Tag::DirObject: {
        auto * self = static_cast<const DirDataNode *>(this);
        auto it = self->entries.find(key);
        if (it == self->entries.end()) return nullptr;
        return new DirScalarNode(it->second);
    }
    case Tag::JsonArray:
    case Tag::JsonString:
    case Tag::JsonNumber:
    case Tag::JsonBool:
    case Tag::JsonNull:
    case Tag::TomlArray:
    case Tag::TomlString:
    case Tag::TomlNumber:
    case Tag::TomlBool:
    case Tag::TomlNull:
    case Tag::DirScalar:
        return nullptr;
    }
    return nullptr;
}

// ── arraySize ───────────────────────────────────────────────────

size_t TracedDataNode::arraySize() const
{
    switch (tag_) {
    case Tag::JsonArray: {
        auto * self = static_cast<const JsonDataNode *>(this);
        return self->data->size();
    }
    case Tag::TomlArray: {
        auto * self = static_cast<const TomlDataNode *>(this);
        return toml::get<std::vector<toml::value>>(*self->data).size();
    }
    case Tag::JsonObject:
    case Tag::JsonString:
    case Tag::JsonNumber:
    case Tag::JsonBool:
    case Tag::JsonNull:
    case Tag::TomlObject:
    case Tag::TomlString:
    case Tag::TomlNumber:
    case Tag::TomlBool:
    case Tag::TomlNull:
    case Tag::DirObject:
    case Tag::DirScalar:
        return 0;
    }
    return 0;
}

// ── arrayGet ────────────────────────────────────────────────────

TracedDataNode * TracedDataNode::arrayGet(size_t index) const
{
    switch (tag_) {
    case Tag::JsonArray: {
        auto * self = static_cast<const JsonDataNode *>(this);
        return new JsonDataNode(&self->data->at(index), self->root);
    }
    case Tag::TomlArray: {
        auto * self = static_cast<const TomlDataNode *>(this);
        return new TomlDataNode(
            &toml::get<std::vector<toml::value>>(*self->data).at(index),
            self->root);
    }
    case Tag::JsonObject:
    case Tag::JsonString:
    case Tag::JsonNumber:
    case Tag::JsonBool:
    case Tag::JsonNull:
    case Tag::TomlObject:
    case Tag::TomlString:
    case Tag::TomlNumber:
    case Tag::TomlBool:
    case Tag::TomlNull:
    case Tag::DirObject:
    case Tag::DirScalar:
        return nullptr;
    }
    return nullptr;
}

// ── materializeScalar ───────────────────────────────────────────

namespace {

void materializeJsonScalar(const JsonDataNode & self, EvalState & state, Value & v)
{
    switch (self.data->type()) {
    case json::value_t::string: {
        auto s = self.data->get<std::string>();
        forceNoNullByte(s);
        v.mkString(s, state.mem);
        return;
    }
    case json::value_t::number_integer:
        v.mkInt(self.data->get<NixInt::Inner>());
        return;
    case json::value_t::number_unsigned: {
        auto val = self.data->get<uint64_t>();
        if (val > static_cast<uint64_t>(std::numeric_limits<NixInt::Inner>::max()))
            throw Error("unsigned json number %1% outside of Nix integer range", val);
        v.mkInt(static_cast<NixInt::Inner>(val));
        return;
    }
    case json::value_t::number_float:
        v.mkFloat(self.data->get<NixFloat>());
        return;
    case json::value_t::boolean:
        v.mkBool(self.data->get<bool>());
        return;
    case json::value_t::null:
    case json::value_t::discarded:
    case json::value_t::binary:
        v.mkNull();
        return;
    case json::value_t::object:
    case json::value_t::array:
        throw Error("cannot materialize non-scalar JSON value");
    }
}

void materializeTomlScalar(const TomlDataNode & self, EvalState & state, Value & v)
{
    switch (self.data->type()) {
    case toml::value_t::boolean:
        v.mkBool(toml::get<bool>(*self.data));
        return;
    case toml::value_t::integer:
        v.mkInt(toml::get<int64_t>(*self.data));
        return;
    case toml::value_t::floating:
        v.mkFloat(toml::get<NixFloat>(*self.data));
        return;
    case toml::value_t::string: {
        auto s = toml::get<std::string_view>(*self.data);
        forceNoNullByte(s);
        v.mkString(s, state.mem);
        return;
    }
    case toml::value_t::local_datetime:
    case toml::value_t::offset_datetime:
    case toml::value_t::local_date:
    case toml::value_t::local_time: {
        if (!experimentalFeatureSettings.isEnabled(Xp::ParseTomlTimestamps))
            throw std::runtime_error("Dates and times are not supported");
        auto mutData = *self.data;
#if HAVE_TOML11_4
        normalizeDatetimeFormatTomlNode(mutData);
#endif
        auto attrs = state.buildBindings(2);
        attrs.alloc("_type").mkStringNoCopy("timestamp"_sds);
        std::ostringstream s;
        s << mutData;
        auto str = s.view();
        forceNoNullByte(str);
        attrs.alloc("value").mkString(str, state.mem);
        v.mkAttrs(attrs);
        return;
    }
    case toml::value_t::empty:
        v.mkNull();
        return;
    case toml::value_t::table:
    case toml::value_t::array:
        throw Error("cannot materialize non-scalar TOML value");
    }
}

void materializeDirScalar(const DirScalarNode & self, Value & v)
{
    if (!self.entryType) {
        v.mkStringNoCopy("unknown"_sds);
        return;
    }
    // Intentionally matches every SourceAccessor::Type variant the
    // existing readDir path produces.  Unknown/unsupported variants
    // ("tChar", "tBlock", "tSocket", etc.) fall through to
    // "unknown" just like the pre-devirt implementation did.
    switch (*self.entryType) {
    case SourceAccessor::tRegular:
        v.mkStringNoCopy("regular"_sds);
        return;
    case SourceAccessor::tDirectory:
        v.mkStringNoCopy("directory"_sds);
        return;
    case SourceAccessor::tSymlink:
        v.mkStringNoCopy("symlink"_sds);
        return;
    // Other SourceAccessor::Type variants (tChar, tBlock, tSocket, …)
    // are handled via the fall-through below.  Silencing
    // -Wswitch-enum here would reintroduce the exhaustive-switch
    // policy; the original readDir code collapsed these to
    // "unknown" via a `default:` case.  Match that behaviour by
    // catching them explicitly.
    case SourceAccessor::tChar:
    case SourceAccessor::tBlock:
    case SourceAccessor::tSocket:
    case SourceAccessor::tFifo:
    case SourceAccessor::tUnknown:
        v.mkStringNoCopy("unknown"_sds);
        return;
    }
}

} // namespace

void TracedDataNode::materializeScalar(EvalState & state, Value & v) const
{
    switch (tag_) {
    case Tag::JsonString:
    case Tag::JsonNumber:
    case Tag::JsonBool:
    case Tag::JsonNull:
        materializeJsonScalar(*static_cast<const JsonDataNode *>(this), state, v);
        return;
    case Tag::TomlString:
    case Tag::TomlNumber:
    case Tag::TomlBool:
    case Tag::TomlNull:
        materializeTomlScalar(*static_cast<const TomlDataNode *>(this), state, v);
        return;
    case Tag::DirScalar:
        materializeDirScalar(*static_cast<const DirScalarNode *>(this), v);
        return;
    case Tag::JsonObject:
    case Tag::JsonArray:
    case Tag::TomlObject:
    case Tag::TomlArray:
    case Tag::DirObject:
        throw Error("cannot materialize non-scalar data node");
    }
}

// ── canonicalValue ──────────────────────────────────────────────

std::string TracedDataNode::canonicalValue() const
{
    switch (tag_) {
    case Tag::JsonObject:
    case Tag::JsonArray:
    case Tag::JsonString:
    case Tag::JsonNumber:
    case Tag::JsonBool:
    case Tag::JsonNull: {
        auto * self = static_cast<const JsonDataNode *>(this);
        return self->data->dump();
    }
    case Tag::TomlObject:
    case Tag::TomlArray:
    case Tag::TomlString:
    case Tag::TomlNumber:
    case Tag::TomlBool:
    case Tag::TomlNull: {
        auto * self = static_cast<const TomlDataNode *>(this);
        return eval_trace::tomlCanonical(*self->data);
    }
    case Tag::DirObject:
        return "";
    case Tag::DirScalar: {
        auto * self = static_cast<const DirScalarNode *>(this);
        return dirEntryTypeString(self->entryType);
    }
    }
    return "";
}

} // namespace nix
