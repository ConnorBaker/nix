#pragma once
///@file

#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <vector>

namespace nix {

struct Value;
class SymbolTable;
class EvalMemory;
struct SourceAccessor;

/**
 * Type tags for the binary serialization format.
 *
 * These tags identify the type of a serialized value. The format is designed
 * to be self-describing so that deserialization doesn't require the original
 * Value.
 */
enum class ValueSerializeTag : uint8_t {
    Null = 0x00,
    BoolFalse = 0x01,
    BoolTrue = 0x02,
    Int = 0x03,
    Float = 0x04,
    String = 0x05,
    Path = 0x06,
    Attrs = 0x07,
    List = 0x08,
};

/**
 * Error thrown when serialization encounters an unsupported value type.
 */
class ValueSerializeError : public std::exception
{
    std::string message;

public:
    explicit ValueSerializeError(std::string msg)
        : message(std::move(msg))
    {
    }

    const char * what() const noexcept override
    {
        return message.c_str();
    }
};

/**
 * Serialize a Nix value to a binary format.
 *
 * The format is designed for cross-evaluation caching, so symbol names are
 * serialized as string bytes, not Symbol IDs (which are session-specific).
 *
 * Supported value types:
 * - Null
 * - Bool
 * - Int (64-bit little-endian)
 * - Float (IEEE 754 double, little-endian)
 * - String (with context)
 * - Path
 * - Attrs (sorted by attribute name)
 * - List
 *
 * Unsupported value types (will throw ValueSerializeError):
 * - Lambda (requires expression serialization)
 * - Thunk (shouldn't be cached - force first!)
 * - PrimOp / PrimOpApp (internal)
 * - External (can't be serialized)
 *
 * @param v The value to serialize. Must be forced (not a thunk).
 * @param symbols Symbol table for resolving attribute names.
 * @return Binary representation of the value.
 * @throws ValueSerializeError if the value type is not supported.
 */
std::vector<uint8_t> serializeValue(const Value & v, const SymbolTable & symbols);

/**
 * Deserialize a binary representation back to a Nix value.
 *
 * This is the inverse of serializeValue(). Symbol names are re-interned
 * into the provided symbol table.
 *
 * @param data Binary data produced by serializeValue().
 * @param v Output value to populate.
 * @param mem Memory allocator for creating new values and bindings.
 * @param symbols Symbol table for interning attribute names.
 * @param pathAccessor Optional accessor for deserialized paths. If nullptr,
 *        paths will be created with null accessors and cannot be used until
 *        fixed up by the caller. For most use cases, you should provide the
 *        root filesystem accessor or the accessor for the evaluation context.
 * @throws ValueSerializeError if the data is malformed.
 */
void deserializeValue(
    std::span<const uint8_t> data,
    Value & v,
    EvalMemory & mem,
    SymbolTable & symbols,
    SourceAccessor * pathAccessor = nullptr);

} // namespace nix
