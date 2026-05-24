#include "nix/util/compression-algo.hh"
#include "nix/util/error.hh"
#include "nix/util/parse-enum.hh"
#include "nix/util/types.hh"

namespace nix {

static const EnumNames<CompressionAlgo> compressionAlgoTable{
    {"none", CompressionAlgo::none},
    {"br", CompressionAlgo::brotli},
    {"bzip2", CompressionAlgo::bzip2},
    {"compress", CompressionAlgo::compress},
    {"grzip", CompressionAlgo::grzip},
    {"gzip", CompressionAlgo::gzip},
    {"lrzip", CompressionAlgo::lrzip},
    {"lz4", CompressionAlgo::lz4},
    {"lzip", CompressionAlgo::lzip},
    {"lzma", CompressionAlgo::lzma},
    {"lzop", CompressionAlgo::lzop},
    {"xz", CompressionAlgo::xz},
    {"zstd", CompressionAlgo::zstd},
};

CompressionAlgo parseCompressionAlgo(std::string_view method)
{
    if (auto v = parseEnumOpt<CompressionAlgo>(method, compressionAlgoTable))
        return *v;

    ErrorInfo err = {.level = lvlError, .msg = HintFmt("unknown compression method '%s'", method)};
    err.suggestions = Suggestions::bestMatches(enumNames<CompressionAlgo>(compressionAlgoTable), method);
    throw UnknownCompressionMethod(std::move(err));
}

std::string showCompressionAlgo(CompressionAlgo method)
{
    for (const auto & entry : compressionAlgoTable)
        if (entry.value == method)
            return std::string(entry.name);
    unreachable();
}

} // namespace nix
