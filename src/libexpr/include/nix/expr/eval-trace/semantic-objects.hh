#pragma once
///@file
///
/// First-class semantic object types for the eval-trace system.
///
/// These types replace the old sidecar provenance model
/// (PathOrigin, ReadFileProvenance, ValuePublication) with typed
/// semantic values that carry provenance as part of their identity
/// rather than as optional metadata.
///
/// SemanticHandle is the carrier type stored on Value objects,
/// replacing the old PathOrigin/ReadFileProvenance fields.

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"
#include "nix/util/types.hh"

#include <optional>
#include <string>

namespace nix {

// ── PathObject ──────────────────────────────────────────────────────
//
// A filesystem path with known semantic origin.  Replaces PathOrigin.

struct PathObject {
    DepSource source;
    CanonPath rootPath;

    bool operator==(const PathObject &) const = default;
};

// ── TextObject ──────────────────────────────────────────────────────
//
// Text content read from a file, with source tracking.  Replaces
// ReadFileProvenance.  Fields flattened from the old ResolvedDepPath.

struct TextObject {
    DepSource source;
    std::string key;
    DepHash contentHash;

    bool operator==(const TextObject &) const = default;
};

// ── StructuredObject ────────────────────────────────────────────────
//
// Parsed structured data (JSON/TOML/readDir) with source tracking.

struct StructuredObject {
    DepSource source;
    std::string key;
    StructuredPath dataPath;
    StructuredFormat format{};

    bool operator==(const StructuredObject &) const = default;
};

// ── IdentityObject ──────────────────────────────────────────────────
//
// Stable semantic identity for replay-safe equality.
//
// For attrs/lists, identity is carried on Bindings (ValueIdentityStamp)
// and checked by sameValueIdentity() in context.cc — this handles the
// case where materialization creates new Bindings*/arrays that break
// pointer equality.  The stamp is assigned at materialization time from
// the trace cache's per-value identity mechanism.
//
// IdentityObject extends this to string/path Values via SemanticHandle,
// where the stamp can be carried on the publication slot.  Used for
// values that need identity-sensitive comparison across materialization
// boundaries where content comparison alone is insufficient.
//
// Round-trip: buildCachedResult (materialize.cc) captures the stamp via
// lookupValueIdentityStamp → pub.withIdentity(IdentityObject{stamp}).
// trace-result-codec serializes/deserializes the stamp as JSON "identity".
// materializeResult (materialize.cc) restores it via setSemanticHandle,
// writing the full SemanticHandle (including identity) to the Value.
// sameValueIdentity (context.cc) checks v.publication()->identity for
// string/path values, enabling identity-sensitive equality across
// materialization boundaries.

struct IdentityObject {
    uint32_t stamp = 0;

    bool operator==(const IdentityObject &) const = default;
    explicit operator bool() const { return stamp != 0; }
};

// ── SemanticKind ────────────────────────────────────────────────────

enum class SemanticKind : uint8_t {
    None = 0,
    Path,           ///< PathObject only.
    Text,           ///< TextObject only.
    PathText,       ///< PathObject + TextObject (readFile under mounted input).
    Structured,     ///< StructuredObject.
};

// ── SemanticHandle ──────────────────────────────────────────────────
//
// Carrier type stored on Value objects.  Replaces the old
// PathOrigin / ReadFileProvenance fields that were attached as
// optional sidecar metadata.

struct SemanticHandle {
    SemanticKind kind = SemanticKind::None;
    std::optional<PathObject> path;
    std::optional<TextObject> text;
    std::optional<StructuredObject> structured;
    std::optional<IdentityObject> identity;

    bool empty() const noexcept { return kind == SemanticKind::None && !identity; }
    bool hasPath() const noexcept { return kind == SemanticKind::Path || kind == SemanticKind::PathText; }
    bool hasText() const noexcept { return kind == SemanticKind::Text || kind == SemanticKind::PathText; }
    bool hasStructured() const noexcept { return kind == SemanticKind::Structured; }
    bool hasIdentity() const noexcept { return identity.has_value() && *identity; }

    static SemanticHandle forPath(PathObject obj)
    {
        return SemanticHandle{
            .kind = SemanticKind::Path,
            .path = std::move(obj),
        };
    }

    static SemanticHandle forText(TextObject obj)
    {
        return SemanticHandle{
            .kind = SemanticKind::Text,
            .text = std::move(obj),
        };
    }

    static SemanticHandle forPathText(PathObject p, TextObject t)
    {
        return SemanticHandle{
            .kind = SemanticKind::PathText,
            .path = std::move(p),
            .text = std::move(t),
        };
    }

    static SemanticHandle forStructured(StructuredObject obj)
    {
        return SemanticHandle{
            .kind = SemanticKind::Structured,
            .structured = std::move(obj),
        };
    }

    /// Attach an IdentityObject to this handle (identity is orthogonal to kind).
    SemanticHandle withIdentity(IdentityObject obj) const
    {
        auto copy = *this;
        copy.identity = std::move(obj);
        return copy;
    }
};

// ── ContextObject ──────────────────────────────────────────────────
//
// Sealed coercion result from string coercion. Three variants:
//   - PlainString: ordinary synthesized string, no provenance
//   - PreservedString: coerced string that preserves semantic provenance
//   - DetachedStorePathString: copied-to-store result, provenance detached
//
// Constructors are private; only EvalState can create instances via
// coerceToContextObject. This sealing ensures provenance decisions
// are made at the coercion boundary, not scattered across builtins.

class EvalState;  // forward declaration for friend access

class ContextObject {
    struct PlainString {
        BackedStringView value;
    };

    struct PreservedString {
        BackedStringView value;
        SemanticHandle publication;
    };

    struct DetachedStorePathString {
        BackedStringView value;
    };

    std::variant<PlainString, PreservedString, DetachedStorePathString> inner_;

    explicit ContextObject(PlainString inner)
        : inner_(std::move(inner))
    {
    }

    explicit ContextObject(PreservedString inner)
        : inner_(std::move(inner))
    {
    }

    explicit ContextObject(DetachedStorePathString inner)
        : inner_(std::move(inner))
    {
    }

    friend class EvalState;

public:
    ContextObject() = delete;
    ContextObject(const ContextObject &) = delete;
    ContextObject & operator=(const ContextObject &) = delete;
    ContextObject(ContextObject &&) noexcept = default;
    ContextObject & operator=(ContextObject &&) noexcept = default;

    [[nodiscard]] bool isDetached() const;

    /// Return the coerced string content.
    ///
    /// The returned view is valid only while this ContextObject is alive.
    /// If the inner BackedStringView owns a std::string, destroying or
    /// moving the ContextObject invalidates the view. Callers must consume
    /// the view within the ContextObject's lifetime scope — do not store
    /// the string_view beyond the enclosing expression or statement.
    [[nodiscard]] std::string_view view() const;
};

} // namespace nix
