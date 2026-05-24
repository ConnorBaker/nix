#pragma once
///@file

#include <cstddef>

namespace nix {

/**
 * Default block size for streaming I/O between sources, sinks, and
 * the kernel. The 64 KiB choice is the page-size-multiple that
 * historically maximised `read(2)`/`write(2)` throughput on Linux
 * across the architectures Nix targets, while still fitting
 * comfortably in L1 caches.
 *
 * Used by:
 *   - `Source::drainInto` (the array that pumps bytes from a
 *     `Source` into a `Sink`)
 *   - `tarfile.cc`'s default libarchive read/write buffer (both
 *     `TarArchive(Source &, …)` and `TarArchive(const path &)`)
 *   - `file-system.cc::writeFile` (Source → file copy buffer)
 *   - `file-descriptor.cc::drainFD` (raw FD drain buffer)
 *   - `file-descriptor.cc::copyFdRange` (raw FD-to-FD copy buffer)
 *
 * Distinct from `tarfile.cc`'s 128 KiB raw-block buffer, which is
 * sized to libarchive's preferred block alignment rather than to
 * page size.
 */
constexpr std::size_t kDefaultIOBlockSize = 65536;

/**
 * Default buffer size for `BufferedSink`/`BufferedSource` and the
 * streaming compression sinks (`compression.cc`'s
 * `ChunkedCompressionSink::outbuf`). The 32 KiB choice is half a
 * page-multiple of `kDefaultIOBlockSize`, small enough that the
 * chunked-codec inner loop can amortise codec state-machine overhead
 * across several iterations per upstream block.
 */
constexpr std::size_t kBufferedStreamSize = 32 * 1024;

} // namespace nix
