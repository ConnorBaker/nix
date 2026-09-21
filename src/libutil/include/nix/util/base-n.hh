#pragma once
///@file

#include <string>
#include <span>

namespace nix {

namespace base16 {

/**
 * Returns the length of a base-16 representation of this many bytes.
 */
[[nodiscard]] constexpr static inline size_t encodedLength(size_t origSize)
{
    return origSize * 2;
}

/**
 * Encode arbitrary bytes as Base16, appending to `out`.  The same characters
 * `encode` returns; the caller's buffer spares the string `encode` makes,
 * which every name of an object file and every fingerprint of a Git node
 * would otherwise allocate and copy (`05` section 6).
 */
void encodeTo(std::string & out, std::span<const std::byte> b);

/**
 * Encode arbitrary bytes as Base16.
 */
std::string encode(std::span<const std::byte> b);

/**
 * Decode arbitrary Base16 string to bytes.
 */
std::string decode(std::string_view s);

} // namespace base16

namespace base64 {

/**
 * Returns the length of a base-64 representation of this many bytes.
 */
[[nodiscard]] constexpr static inline size_t encodedLength(size_t origSize)
{
    return ((4 * origSize / 3) + 3) & ~3;
}

/**
 * Encode arbitrary bytes as Base64, appending to `out`.
 */
void encodeTo(std::string & out, std::span<const std::byte> b);

/**
 * Encode arbitrary bytes as Base64.
 */
std::string encode(std::span<const std::byte> b);

/**
 * Decode arbitrary Base64 string to bytes.
 */
std::string decode(std::string_view s);

} // namespace base64

} // namespace nix
