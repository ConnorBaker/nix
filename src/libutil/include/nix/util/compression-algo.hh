#pragma once
///@file

#include "nix/util/error.hh"

#include <string_view>

namespace nix {

enum class CompressionAlgo {
    none,
    brotli,
    bzip2,
    compress,
    grzip,
    gzip,
    lrzip,
    lz4,
    lzip,
    lzma,
    lzop,
    xz,
    zstd,
};

/**
 * Parses a *compression* method into the corresponding enum. This is only used
 * in the *compression* case and user interface. Content-Encoding should not use
 * these. Throws `UnknownCompressionMethod` populated with best-match
 * suggestions on no match.
 */
CompressionAlgo parseCompressionAlgo(std::string_view method);

std::string showCompressionAlgo(CompressionAlgo method);

MakeError(UnknownCompressionMethod, Error);

} // namespace nix
