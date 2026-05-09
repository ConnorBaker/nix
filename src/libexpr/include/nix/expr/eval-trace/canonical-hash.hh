#pragma once

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"
#include "nix/util/tagged.hh"

#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace nix::eval_trace {

class CanonicalHashBuilder
{
    HashSink sink_;

    void writeBytes(std::string_view bytes)
    {
        sink_(bytes);
    }

    void writeLength(uint64_t len)
    {
        std::array<char, sizeof(len)> bytes{};
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<char>((len >> (i * 8)) & 0xff);
        writeBytes({bytes.data(), bytes.size()});
    }

    void writeFrame(std::string_view tag, std::string_view payload)
    {
        writeLength(tag.size());
        writeBytes(tag);
        writeLength(payload.size());
        writeBytes(payload);
    }

public:
    /// Primary (pure) constructor: algorithm is an explicit argument,
    /// so two builders with the same domain + algorithm always produce
    /// the same preimage regardless of process-global settings.
    explicit CanonicalHashBuilder(std::string_view domain, EvalTraceHashAlgorithm algorithm)
        : sink_(toHashAlgorithm(algorithm))
    {
        field("$domain", domain);
    }

    /// Compatibility hatch: reads the process-global
    /// `getEvalTraceHashAlgorithm()` at construction time. Prefer the
    /// explicit-algorithm overload in new code (ref adversarial
    /// review #8).
    explicit CanonicalHashBuilder(std::string_view domain)
        : CanonicalHashBuilder(domain, getEvalTraceHashAlgorithm())
    {
    }

    void field(std::string_view tag, std::string_view value)
    {
        writeFrame(tag, value);
    }

    void field(std::string_view tag, const EvalTraceHash & value)
    {
        writeFrame(tag, value.view());
    }

    /// Unwrap-and-forward for any Tagged<_, T> whose inner T has its own
    /// field() overload.  Replaces the four hand-rolled Tagged overloads
    /// for EvalTraceHash, std::string, CanonPath (and any future inner
    /// types that get their own overload).
    template<typename Tag, typename T>
    void field(std::string_view tag, const Tagged<Tag, T> & value)
    {
        field(tag, value.value);
    }

    void field(std::string_view tag, const CanonPath & value)
    {
        field(tag, std::string_view(value.abs()));
    }

    template<typename Tag>
    void field(std::string_view tag, const Tagged<Tag, std::string> & value)
    {
        field(tag, std::string_view(value.value));
    }

    template<std::unsigned_integral UInt>
    void field(std::string_view tag, UInt value)
    {
        std::array<char, sizeof(UInt)> bytes{};
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<char>((value >> (i * 8)) & 0xff);
        writeFrame(tag, {bytes.data(), bytes.size()});
    }

    template<std::signed_integral SInt>
    void field(std::string_view tag, SInt value)
    {
        using UInt = std::make_unsigned_t<SInt>;
        field(tag, static_cast<UInt>(value));
    }

    template<typename Enum>
        requires std::is_enum_v<Enum>
    void field(std::string_view tag, Enum value)
    {
        field(tag, std::to_underlying(value));
    }

    void field(std::string_view tag, bool value)
    {
        field(tag, static_cast<uint8_t>(value ? 1 : 0));
    }

    template<typename T>
    void optionalField(std::string_view tag, const std::optional<T> & value)
    {
        field(std::string(tag) + ".present", value.has_value());
        if (value)
            field(tag, *value);
    }

    EvalTraceHash finish()
    {
        return EvalTraceHash::fromHash(sink_.finish().hash);
    }
};

template<typename Tag>
inline std::string taggedEvalTraceHashHex(const Tagged<Tag, EvalTraceHash> & value)
{
    return value.value.toHex();
}

/// Compile-time domain tags for CanonicalHashBuilder.
///
/// Each tag exposes a `constexpr std::string_view domain` that names the
/// hash preimage's logical namespace. Using a tag type (instead of a raw
/// string literal) at construction sites turns a typo in any domain string
/// into a compile error — `makeDomainBuilder<Tag>()` fails to compile if
/// `Tag::domain` is not a string_view-convertible constant.
///
/// Adding a new domain is additive: declare the struct here, then use
/// `makeDomainBuilder<hash_domain::NewDomain>()` at the hashing site.
namespace hash_domain {

// ── hash.hh (sorted-deps hash computation) ────────────────────────
struct TraceHashV2     { static constexpr std::string_view domain = "eval-trace.trace-hash.v2"; };
struct FullTraceHashV1 { static constexpr std::string_view domain = "eval-trace.full-trace-hash.v1"; };
struct StructHashV2    { static constexpr std::string_view domain = "eval-trace.trace-struct-hash.v2"; };
struct DepKeySetHashV1 { static constexpr std::string_view domain = "eval-trace.dep-key-set-hash.v1"; };

// ── result + policy ───────────────────────────────────────────────
struct ResultHashV2         { static constexpr std::string_view domain = "eval-trace.result-hash.v2"; };
struct PolicyDigestSettings { static constexpr std::string_view domain = "eval-trace.policy-digest.settings"; };
struct PolicySnapshot       { static constexpr std::string_view domain = "eval-policy-snapshot"; };

// ── dep / shape hashing ───────────────────────────────────────────
struct CanonicalKeysHash     { static constexpr std::string_view domain = "canonical-keys-hash"; };
struct DepHashDirListing     { static constexpr std::string_view domain = "dep-hash-dir-listing"; };
struct GitIdentity           { static constexpr std::string_view domain = "git-identity"; };
struct NixBindingScopeHash   { static constexpr std::string_view domain = "nix-binding.scope-hash"; };
struct NixBindingBindingHash { static constexpr std::string_view domain = "nix-binding.binding-hash"; };
struct RuntimeRootSourceKey  { static constexpr std::string_view domain = "runtime-root-source-key"; };

// ── session / recovery ────────────────────────────────────────────
struct SemanticSessionKeySeed { static constexpr std::string_view domain = "semantic-session-key-seed"; };
struct SemanticSessionKey     { static constexpr std::string_view domain = "semantic-session-key"; };
struct TestStableRecoveryKey  { static constexpr std::string_view domain = "test-stable-recovery-key"; };

// ── flake-layer domains ───────────────────────────────────────────
struct FlakeLockedVersionIdentity         { static constexpr std::string_view domain = "flake-locked-version-identity"; };
struct FlakeRelativeLockedVersionIdentity { static constexpr std::string_view domain = "flake-relative-locked-version-identity"; };
struct FlakeResolvedGraphDigest           { static constexpr std::string_view domain = "flake-resolved-graph-digest"; };
struct LockedFlakeFingerprint             { static constexpr std::string_view domain = "locked-flake-fingerprint"; };
struct FlakeStableRecoveryKeyDomain       { static constexpr std::string_view domain = "flake-stable-recovery-key"; };
struct FlakeSourceIdentityDomain          { static constexpr std::string_view domain = "flake-source-identity"; };

// ── file-eval session-key variants ────────────────────────────────
struct FileEvalSourceIdentity    { static constexpr std::string_view domain = "file-eval-source-identity"; };
struct FileEvalStableRecoveryKey { static constexpr std::string_view domain = "file-eval-stable-recovery-key"; };
struct FileEvalSessionReuseKey   { static constexpr std::string_view domain = "file-eval-session-reuse-key"; };

} // namespace hash_domain

/// Construct a CanonicalHashBuilder whose domain is a compile-time constant
/// pulled from the Tag's `domain` member. A typo anywhere becomes a compile
/// error (the tag is a type, not a string).
///
/// Primary (pure) overload takes the algorithm explicitly. The
/// argument-less overload is a compatibility hatch that reads
/// `getEvalTraceHashAlgorithm()` (ref adversarial review #8).
template<typename Tag>
inline CanonicalHashBuilder makeDomainBuilder(EvalTraceHashAlgorithm algorithm)
{
    static_assert(
        std::is_convertible_v<decltype(Tag::domain), std::string_view>,
        "makeDomainBuilder<Tag>: Tag must expose a "
        "constexpr std::string_view domain member");
    return CanonicalHashBuilder(Tag::domain, algorithm);
}

template<typename Tag>
inline CanonicalHashBuilder makeDomainBuilder()
{
    return makeDomainBuilder<Tag>(getEvalTraceHashAlgorithm());
}

} // namespace nix::eval_trace
