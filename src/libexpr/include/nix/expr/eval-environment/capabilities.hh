#pragma once

#include "nix/expr/eval-environment/domains.hh"
#include "nix/expr/eval-environment/fwd.hh"
#include "nix/expr/eval-trace/semantic-objects.hh"
#include "nix/util/conditional-base.hh"
#include "nix/util/linear.hh"
#include "nix/util/move-only.hh"
#include "nix/util/repair-flag.hh"
#include "nix/expr/value/context.hh"
#include "nix/util/ref.hh"
#include "nix/util/source-accessor.hh"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace nix {

/**
 * Zero-sized overload selectors for API mode choice.
 *
 * These are not capabilities and do not carry authority. They are intentionally
 * forgeable and only select the legality/recording mode of a call:
 *
 * - `ObserveOnlyTag`: read-only observation, no publication/materialization
 */
struct ObserveOnlyTag final
{
    explicit constexpr ObserveOnlyTag() = default;
};

inline constexpr ObserveOnlyTag observeOnly{};

enum class EffectMode : uint8_t {
    Detached,
    Bound,
};

/// Field present only on bound effect scopes.
struct BoundSessionField {
    ref<eval_trace::TraceSession> session_;
};

/**
 * Opaque capability proving that environment effects are legal.
 *
 * Capabilities in this header are authority/context objects, not data models.
 * They are minted only by trusted environment/session code so callers cannot
 * fake detached mutation or session-bound recording authority by constructing
 * ordinary values.
 *
 * - `EffectScope<Detached>` (aka `DetachedEffectScope`): shared libexpr
 *   mutation outside a bound trace session.
 * - `EffectScope<Bound>` (aka `BoundEffectScope`): effects that may record
 *   against a specific trace session.
 */
template <EffectMode E>
class EffectScope final
    : private MoveOnly
    , private ConditionalBase<E == EffectMode::Bound, BoundSessionField>
{
    static_assert(E == EffectMode::Detached || E == EffectMode::Bound);

    std::shared_ptr<void> state_;

    EffectScope() = default;

    explicit EffectScope(std::shared_ptr<void> state)
        requires (E == EffectMode::Detached)
        : state_(std::move(state))
    {}

    EffectScope(std::shared_ptr<void> state, ref<eval_trace::TraceSession> session)
        requires (E == EffectMode::Bound)
        : BoundSessionField{std::move(session)}
        , state_(std::move(state))
    {}

    friend class EvalEnvironment;
};

using DetachedEffectScope = EffectScope<EffectMode::Detached>;
using BoundEffectScope    = EffectScope<EffectMode::Bound>;

enum class LookupPathEntryDetail : uint8_t { Identity, Full };
enum class LookupPathRealization : uint8_t { Unrealized, Realized };

/// Fields present only on full lookup path entries (not identity-only).
struct LookupPathEntryContextFields {
    NixStringContext context;
    std::optional<PathObject> origin;
};

/// Parameterized lookup path entry.
///
/// Two axes:
/// - Detail: Full entries carry context + origin; Identity entries carry
///   only prefix + rawValue.
/// - Realization: Unrealized entries may have unresolved context placeholders
///   in rawValue; Realized entries have a final rawValue safe for dep identity.
///
/// `toIdentity()` is always available on Full entries — it strips context/origin.
/// The realization state is preserved: a Full/Unrealized entry produces an
/// Identity/Unrealized entry (for session reuse keys), while a Full/Realized
/// entry produces an Identity/Realized entry (for dep recording).
///
/// `realize()` transitions Full/Unrealized → Full/Realized, optionally
/// rewriting rawValue. Identity entries cannot be constructed from scratch —
/// only through toIdentity() on a Full entry.
template <LookupPathEntryDetail D, LookupPathRealization R = LookupPathRealization::Unrealized>
class LookupPathEntry
    : public ConditionalBase<D == LookupPathEntryDetail::Full, LookupPathEntryContextFields>
{
    static_assert(D == LookupPathEntryDetail::Identity || D == LookupPathEntryDetail::Full);
    static_assert(R == LookupPathRealization::Unrealized || R == LookupPathRealization::Realized);

    // Cross-instantiation friend: Full entries construct Identity entries
    // via toIdentity(), and realize() constructs Realized from Unrealized.
    template <LookupPathEntryDetail, LookupPathRealization>
    friend class LookupPathEntry;

    // The sole entry point for constructing Full/Unrealized entries.
    friend LookupPathEntry<LookupPathEntryDetail::Full, LookupPathRealization::Unrealized>
    buildLookupPathEntrySpec(
        LookupPathEntryContextFields contextFields,
        std::optional<LookupPathPrefix> prefix,
        LookupPathRawValue rawValue);

public:
    std::optional<LookupPathPrefix> prefix;
    LookupPathRawValue rawValue;

    /// Strip context/origin, preserving realization state.
    LookupPathEntry<LookupPathEntryDetail::Identity, R> toIdentity() const
        requires (D == LookupPathEntryDetail::Full)
    {
        LookupPathEntry<LookupPathEntryDetail::Identity, R> result;
        result.prefix = prefix;
        result.rawValue = rawValue;
        return result;
    }

    /// Realize context: transition to Realized state.
    /// For entries with empty context, call with no argument to keep rawValue.
    /// For entries with non-empty context, pass the rewritten rawValue.
    LookupPathEntry<D, LookupPathRealization::Realized> realize() const
        requires (D == LookupPathEntryDetail::Full && R == LookupPathRealization::Unrealized)
    {
        LookupPathEntry<D, LookupPathRealization::Realized> result;
        static_cast<LookupPathEntryContextFields &>(result) =
            static_cast<const LookupPathEntryContextFields &>(*this);
        result.prefix = prefix;
        result.rawValue = rawValue;
        return result;
    }

    LookupPathEntry<D, LookupPathRealization::Realized> realize(
        LookupPathRawValue realizedRawValue) const
        requires (D == LookupPathEntryDetail::Full && R == LookupPathRealization::Unrealized)
    {
        LookupPathEntry<D, LookupPathRealization::Realized> result;
        static_cast<LookupPathEntryContextFields &>(result) =
            static_cast<const LookupPathEntryContextFields &>(*this);
        result.prefix = prefix;
        result.rawValue = std::move(realizedRawValue);
        return result;
    }

private:
    LookupPathEntry() = default;
};

/// Construct a full unrealized lookup path entry.
inline LookupPathEntry<LookupPathEntryDetail::Full, LookupPathRealization::Unrealized>
buildLookupPathEntrySpec(
    LookupPathEntryContextFields contextFields,
    std::optional<LookupPathPrefix> prefix,
    LookupPathRawValue rawValue)
{
    LookupPathEntry<LookupPathEntryDetail::Full, LookupPathRealization::Unrealized> entry;
    static_cast<LookupPathEntryContextFields &>(entry) = std::move(contextFields);
    entry.prefix = std::move(prefix);
    entry.rawValue = std::move(rawValue);
    return entry;
}

/// Type alias naming convention: <Tag><Template>.
/// The tag prefix matches the enum enumerator name.
/// Example: BoundEffectScope = EffectScope<EffectMode::Bound>

/// Full unrealized entry — the default construction target.
using UnrealizedFullLookupPathEntry = LookupPathEntry<LookupPathEntryDetail::Full, LookupPathRealization::Unrealized>;
/// Identity extracted from an unrealized entry — for session reuse keys.
using UnrealizedLookupPathIdentity = LookupPathEntry<LookupPathEntryDetail::Identity, LookupPathRealization::Unrealized>;
/// Identity extracted from a realized entry — for dep recording.
using RealizedLookupPathIdentity = LookupPathEntry<LookupPathEntryDetail::Identity, LookupPathRealization::Realized>;

struct EvalEnvironmentSharedState;

struct EvalEnvironmentAuthority
{
    EvalState * evalState;
    ref<Store> store;
    ref<Store> buildStore;
    const fetchers::Settings & fetchSettings;
    const EvalSettings & evalSettings;
    RepairFlag repair = NoRepair;
    ref<MountedSourceAccessor> storeFS;
    ref<SourceAccessor> rootFS;
    ref<MemorySourceAccessor> corepkgsFS;
    ref<MemorySourceAccessor> internalFS;
    ref<fetchers::InputCache> inputCache;
    std::function<std::optional<SourcePath>(std::string_view, std::string_view)> lookupPathHookResolver;
    std::shared_ptr<TraceSessionFactory> traceSessionFactory;
    std::shared_ptr<EvalEnvironmentSharedState> sharedState;
};

class RootLoaderHolder
{
public:
    virtual ~RootLoaderHolder() = default;
    virtual Value * loadRoot() = 0;
};

/**
 * One-shot capability for converting a captured root-loader holder into the
 * callable root loader used by a `TraceSession`.
 *
 * This is `Linear` rather than `MoveOnly` because silently dropping a prepared
 * root loader would hide a bug in session assembly.
 */
class RootLoaderCapability final : public Linear<RootLoaderCapability>
{
    std::unique_ptr<RootLoaderHolder> holder_;

    explicit RootLoaderCapability(
        std::unique_ptr<RootLoaderHolder> holder)
        : holder_(std::move(holder))
    {
    }

    friend class EvalEnvironment;

public:
    static constexpr const char * linearName = "RootLoaderCapability";

    static RootLoaderCapability create(std::unique_ptr<RootLoaderHolder> holder)
    {
        return RootLoaderCapability(std::move(holder));
    }

    void discardUnused() &&
    {
        holder_.reset();
        this->markConsumed();
    }

    std::function<Value *()> intoRootLoader() &&
    {
        this->markConsumed();

        struct State {
            std::unique_ptr<RootLoaderHolder> holder;
        };

        auto state = std::make_shared<State>(State{
            .holder = std::move(holder_),
        });

        return [state]() mutable -> Value * {
            return state->holder->loadRoot();
        };
    }
};

} // namespace nix
