#include "nix/expr/value-serialize.hh"

#include "nix/expr/attr-set.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/expr/value/context.hh"
#include "nix/util/source-accessor.hh"

#include <algorithm>
#include <bit>
#include <cstring>

namespace nix {

namespace {

/**
 * Helper class for building serialized output.
 */
class SerializeBuffer
{
    std::vector<uint8_t> buffer;

public:
    void writeByte(uint8_t b)
    {
        buffer.push_back(b);
    }

    void writeTag(ValueSerializeTag tag)
    {
        writeByte(static_cast<uint8_t>(tag));
    }

    void writeBytes(const uint8_t * data, size_t len)
    {
        buffer.insert(buffer.end(), data, data + len);
    }

    /**
     * Write a 64-bit integer in little-endian format.
     */
    void writeInt64(int64_t val)
    {
        uint64_t uval = static_cast<uint64_t>(val);
        for (int i = 0; i < 8; ++i) {
            writeByte(static_cast<uint8_t>(uval & 0xFF));
            uval >>= 8;
        }
    }

    /**
     * Write a 64-bit unsigned integer in little-endian format.
     */
    void writeUInt64(uint64_t val)
    {
        for (int i = 0; i < 8; ++i) {
            writeByte(static_cast<uint8_t>(val & 0xFF));
            val >>= 8;
        }
    }

    /**
     * Write an IEEE 754 double in little-endian format.
     */
    void writeDouble(double val)
    {
        uint64_t bits = std::bit_cast<uint64_t>(val);
        writeUInt64(bits);
    }

    /**
     * Write a length-prefixed string.
     * Format: 8-byte little-endian length + raw bytes (no null terminator).
     */
    void writeString(std::string_view s)
    {
        writeUInt64(s.size());
        writeBytes(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }

    std::vector<uint8_t> finish()
    {
        return std::move(buffer);
    }
};

/**
 * Helper class for reading serialized data.
 */
class DeserializeBuffer
{
    std::span<const uint8_t> data;
    size_t pos = 0;

public:
    explicit DeserializeBuffer(std::span<const uint8_t> data)
        : data(data)
    {
    }

    bool empty() const
    {
        return pos >= data.size();
    }

    size_t remaining() const
    {
        return data.size() - pos;
    }

    uint8_t readByte()
    {
        if (pos >= data.size()) {
            throw ValueSerializeError("unexpected end of data");
        }
        return data[pos++];
    }

    ValueSerializeTag readTag()
    {
        return static_cast<ValueSerializeTag>(readByte());
    }

    int64_t readInt64()
    {
        uint64_t uval = 0;
        for (int i = 0; i < 8; ++i) {
            uval |= static_cast<uint64_t>(readByte()) << (i * 8);
        }
        return static_cast<int64_t>(uval);
    }

    uint64_t readUInt64()
    {
        uint64_t val = 0;
        for (int i = 0; i < 8; ++i) {
            val |= static_cast<uint64_t>(readByte()) << (i * 8);
        }
        return val;
    }

    double readDouble()
    {
        uint64_t bits = readUInt64();
        return std::bit_cast<double>(bits);
    }

    std::string_view readString()
    {
        uint64_t len = readUInt64();
        if (len > remaining()) {
            throw ValueSerializeError("string length exceeds remaining data");
        }
        const char * start = reinterpret_cast<const char *>(data.data() + pos);
        pos += len;
        return std::string_view(start, len);
    }
};

/**
 * Serialize a value recursively.
 */
void serializeValueImpl(SerializeBuffer & buf, const Value & v, const SymbolTable & symbols)
{
    switch (v.type()) {
    case nNull:
        buf.writeTag(ValueSerializeTag::Null);
        break;

    case nBool:
        buf.writeTag(v.boolean() ? ValueSerializeTag::BoolTrue : ValueSerializeTag::BoolFalse);
        break;

    case nInt:
        buf.writeTag(ValueSerializeTag::Int);
        buf.writeInt64(v.integer().value);
        break;

    case nFloat:
        buf.writeTag(ValueSerializeTag::Float);
        buf.writeDouble(v.fpoint());
        break;

    case nString: {
        buf.writeTag(ValueSerializeTag::String);
        // Write the string content
        buf.writeString(v.string_view());

        // Write string context
        auto * ctx = v.context();
        if (ctx && ctx->size() > 0) {
            buf.writeUInt64(ctx->size());
            for (auto it = ctx->begin(); it != ctx->end(); ++it) {
                // Each context element is a StringData pointer; write its string content
                buf.writeString((*it)->view());
            }
        } else {
            buf.writeUInt64(0);
        }
        break;
    }

    case nPath: {
        buf.writeTag(ValueSerializeTag::Path);
        // Serialize the path string
        // Note: We only serialize the path string, not the accessor.
        // During deserialization, paths will need to be resolved in a new context.
        buf.writeString(v.pathStrView());
        break;
    }

    case nAttrs: {
        buf.writeTag(ValueSerializeTag::Attrs);
        const Bindings * attrs = v.attrs();

        // Get attributes in lexicographic order for deterministic serialization
        auto sorted = attrs->lexicographicOrder(symbols);

        buf.writeUInt64(sorted.size());
        for (const Attr * attr : sorted) {
            // Write attribute name as string bytes (not Symbol ID!)
            std::string_view name = symbols[attr->name];
            buf.writeString(name);

            // Recursively serialize the attribute value
            serializeValueImpl(buf, *attr->value, symbols);
        }
        break;
    }

    case nList: {
        buf.writeTag(ValueSerializeTag::List);
        auto listView = v.listView();
        buf.writeUInt64(listView.size());
        for (Value * elem : listView) {
            serializeValueImpl(buf, *elem, symbols);
        }
        break;
    }

    case nThunk:
        throw ValueSerializeError(
            "cannot serialize thunk - force the value first! "
            "Thunks represent unevaluated expressions that may have different "
            "results in different evaluation contexts.");

    case nFunction:
        throw ValueSerializeError(
            "cannot serialize function - functions contain closures that "
            "reference runtime environments and cannot be meaningfully "
            "persisted across evaluations.");

    case nExternal:
        throw ValueSerializeError(
            "cannot serialize external value - external values are "
            "opaque C++ objects that cannot be serialized.");

    default:
        throw ValueSerializeError("unknown value type: " + std::to_string(static_cast<int>(v.type())));
    }
}

/**
 * Deserialize a value recursively.
 */
void deserializeValueImpl(
    DeserializeBuffer & buf, Value & v, EvalMemory & mem, SymbolTable & symbols, SourceAccessor * pathAccessor)
{
    ValueSerializeTag tag = buf.readTag();

    switch (tag) {
    case ValueSerializeTag::Null:
        v.mkNull();
        break;

    case ValueSerializeTag::BoolFalse:
        v.mkBool(false);
        break;

    case ValueSerializeTag::BoolTrue:
        v.mkBool(true);
        break;

    case ValueSerializeTag::Int:
        v.mkInt(buf.readInt64());
        break;

    case ValueSerializeTag::Float:
        v.mkFloat(buf.readDouble());
        break;

    case ValueSerializeTag::String: {
        std::string_view str = buf.readString();
        uint64_t contextSize = buf.readUInt64();

        if (contextSize == 0) {
            // String without context
            v.mkString(str, mem);
        } else {
            // String with context - rebuild the NixStringContext
            NixStringContext context;
            for (uint64_t i = 0; i < contextSize; ++i) {
                std::string_view ctxStr = buf.readString();
                // Parse the context element
                context.insert(NixStringContextElem::parse(ctxStr));
            }
            v.mkString(str, context, mem);
        }
        break;
    }

    case ValueSerializeTag::Path: {
        std::string_view pathStr = buf.readString();
        // Create a path value using the provided accessor.
        // If pathAccessor is null, the path will have a null accessor and
        // cannot be used until fixed up by the caller.
        auto & pathData = StringData::alloc(mem, pathStr.size());
        std::memcpy(pathData.data(), pathStr.data(), pathStr.size());
        pathData.data()[pathStr.size()] = '\0';
        v.mkPath(pathAccessor, pathData);
        break;
    }

    case ValueSerializeTag::Attrs: {
        uint64_t size = buf.readUInt64();
        auto builder = mem.buildBindings(symbols, size);

        for (uint64_t i = 0; i < size; ++i) {
            // Read attribute name and intern it
            std::string_view name = buf.readString();
            Symbol sym = symbols.create(name);

            // Allocate and deserialize the value
            Value * attrValue = mem.allocValue();
            deserializeValueImpl(buf, *attrValue, mem, symbols, pathAccessor);

            builder.insert(sym, attrValue);
        }

        v.mkAttrs(builder.finish());
        break;
    }

    case ValueSerializeTag::List: {
        uint64_t size = buf.readUInt64();

        if (size == 0) {
            v = Value::vEmptyList;
        } else {
            auto builder = mem.buildList(size);
            for (uint64_t i = 0; i < size; ++i) {
                builder[i] = mem.allocValue();
                deserializeValueImpl(buf, *builder[i], mem, symbols, pathAccessor);
            }
            v.mkList(builder);
        }
        break;
    }

    default:
        throw ValueSerializeError("unknown serialization tag: " + std::to_string(static_cast<uint8_t>(tag)));
    }
}

} // anonymous namespace

std::vector<uint8_t> serializeValue(const Value & v, const SymbolTable & symbols)
{
    SerializeBuffer buf;
    serializeValueImpl(buf, v, symbols);
    return buf.finish();
}

void deserializeValue(
    std::span<const uint8_t> data, Value & v, EvalMemory & mem, SymbolTable & symbols, SourceAccessor * pathAccessor)
{
    DeserializeBuffer buf(data);
    deserializeValueImpl(buf, v, mem, symbols, pathAccessor);

    if (!buf.empty()) {
        throw ValueSerializeError(
            "extra data after deserialized value (" + std::to_string(buf.remaining()) + " bytes remaining)");
    }
}

} // namespace nix
