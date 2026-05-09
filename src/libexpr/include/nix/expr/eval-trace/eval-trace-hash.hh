#pragma once
///@file
///
/// Fixed-size eval-trace digest and its HashSink-backed builder.
///
/// Split out of the larger `deps/types.hh` so lightweight headers
/// (session-identity.hh, future abstract-TraceStorage, …) can depend
/// on the digest type without pulling in the whole dep-type surface
/// (~1200 lines of CanonicalQueryKind, Dep, Dep::Key, DepHashValue,
/// serialization helpers, etc.).

#include "nix/expr/eval-trace/hash-spec.hh"
#include "nix/util/hash.hh"
#include "nix/util/tagged.hh"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace nix {

/**
 * Fixed-size eval-trace digest. Stack-allocated, no heap allocation.
 * The active hash backend is runtime-selectable, but every supported
 * backend currently produces a 256-bit digest.
 */
struct EvalTraceHash {
    std::array<uint8_t, eval_trace::kEvalTraceDigestSize> bytes{};

    bool operator==(const EvalTraceHash &) const = default;
    auto operator<=>(const EvalTraceHash &) const = default;

    const unsigned char * data() const { return bytes.data(); }
    static constexpr size_t size() { return eval_trace::kEvalTraceDigestSize; }

    /** View as string_view for feeding into HashSink. */
    std::string_view view() const {
        return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    }

    /** Construct from a Hash object (asserts hashSize matches eval-trace digest size). */
    static EvalTraceHash fromHash(const Hash & h) {
        EvalTraceHash result;
        assert(h.hashSize == eval_trace::kEvalTraceDigestSize);
        std::memcpy(result.bytes.data(), h.hash, result.bytes.size());
        return result;
    }

    /** Construct from a raw BLOB pointer. */
    static EvalTraceHash fromBlob(const void * data, size_t len) {
        EvalTraceHash result;
        assert(len == eval_trace::kEvalTraceDigestSize);
        std::memcpy(result.bytes.data(), data, result.bytes.size());
        return result;
    }

    /** For use as unordered_map key — first 8 bytes are already well-distributed. */
    struct Hasher {
        size_t operator()(const EvalTraceHash & h) const noexcept {
            uint64_t v;
            std::memcpy(&v, h.bytes.data(), sizeof(v));
            return v;
        }
    };

    /** Format as hex for logging (debug path only). */
    std::string toHex() const {
        static constexpr char hex[] = "0123456789abcdef";
        std::string out(bytes.size() * 2, '\0');
        for (size_t i = 0; i < bytes.size(); i++) {
            out[2 * i] = hex[bytes[i] >> 4];
            out[2 * i + 1] = hex[bytes[i] & 0xf];
        }
        return out;
    }
};

/// Construct a phantom-typed EvalTraceHash from a HashSink result.
/// Template parameter is the alias type (e.g., TraceHash), not the tag.
template<typename T>
T evalTraceHashFromSink(HashSink & sink) {
    return {EvalTraceHash::fromHash(sink.finish().hash)};
}

/// Construct a phantom-typed EvalTraceHash from a raw BLOB (e.g., SQLite read).
template<typename T>
T evalTraceHashFromBlob(const void * data, size_t len) {
    return {EvalTraceHash::fromBlob(data, len)};
}

/**
 * Convenience eval-trace hasher. Wraps HashSink and produces
 * EvalTraceHash or a phantom-typed EvalTraceHash directly using a
 * specific hash backend.
 *
 * Construction takes the algorithm explicitly. The default arg
 * reads the process-global `getEvalTraceHashAlgorithm()` — this is a
 * compatibility hatch for existing call sites; prefer the explicit
 * form in new code (ref adversarial review #8).
 */
struct EvalTraceHasher {
    HashSink sink;

    explicit EvalTraceHasher(
        eval_trace::EvalTraceHashAlgorithm algorithm = eval_trace::getEvalTraceHashAlgorithm())
        : sink(eval_trace::toHashAlgorithm(algorithm)) {}

    EvalTraceHasher & operator()(std::string_view data) { sink(data); return *this; }

    EvalTraceHash finish() { return EvalTraceHash::fromHash(sink.finish().hash); }

    template<typename T>
    T finishAs() { return {finish()}; }
};

// ── Phantom-typed eval-trace hashes ──────────────────────────────────
//
// TraceHash, StructHash, ResultHash, FullTraceHash, and DepKeySetHash are
// Tagged<Tag, EvalTraceHash> — plain phantom types preventing
// cross-domain hash confusion.

using DepHash = Tagged<struct DepHashTag_, EvalTraceHash>;        ///< hash of dependency-observed data
using TraceHash = Tagged<struct TraceHashTag_, EvalTraceHash>;    ///< canonical recovery hash of contributing dep keys + values
using StructHash = Tagged<struct StructHashTag_, EvalTraceHash>;  ///< canonical recovery hash of contributing dep keys only
using ResultHash = Tagged<struct ResultHashTag_, EvalTraceHash>;  ///< hash of result type + value + context
using FullTraceHash = Tagged<struct FullTraceHashTag_, EvalTraceHash>; ///< storage identity hash of all dep keys + values
using DepKeySetHash = Tagged<struct DepKeySetHashTag_, EvalTraceHash>; ///< storage identity hash of all dep keys only

/// Phantom-typed git identity hashes to prevent stored/current confusion (BUG-1).
/// StoredGitIdentityHash: extracted from a trace's deps (represents git state
/// at recording time). CurrentGitIdentityHash: computed from the current workdir
/// (represents git state right now).
using StoredGitIdentityHash = Tagged<struct StoredGitIdentityTag_, EvalTraceHash>;
using CurrentGitIdentityHash = Tagged<struct CurrentGitIdentityTag_, EvalTraceHash>;

} // namespace nix
