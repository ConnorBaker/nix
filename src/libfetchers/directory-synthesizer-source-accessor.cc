#include "nix/fetchers/directory-synthesizer-source-accessor.hh"

namespace nix {

std::optional<SourceAccessor::Stat> DirectorySynthesizerSourceAccessor::maybeLstat(const CanonPath & p)
{
    /* Consult `next` first — composes correctly with stacked operators
       (e.g. when D wraps a filter that already returned a Some). */
    if (auto st = next->maybeLstat(p))
        return st;
    /* `next` lacks the path. If `p` is a strict ancestor of some
       `s ∈ S`, synthesise a directory stat so callers can walk
       toward accepted leaves. `pathSet::ancestorOfMember` excludes
       the equality case (p ∉ S), and the empty-S case is handled by
       the factory's identity short-circuit (D never wraps for empty
       paths). */
    if (pathSet::ancestorOfMember(*paths, p))
        return Stat{.type = SourceAccessor::tDirectory};
    return std::nullopt;
}

SourceAccessor::DirEntries DirectorySynthesizerSourceAccessor::readDirectory(const CanonPath & p)
{
    /* Real children from next, suppressing FileNotFound: synthesis
       can produce a directory `next` doesn't know about (e.g. when
       `next` is a Restrict that filtered everything out at p, or an
       overlay accessor that lacks the path entirely). */
    DirEntries result;
    try {
        result = next->readDirectory(p);
    } catch (FileNotFound &) {
    }

    /* Add intermediate-dir children synthesised from S. For each
       s in S, walk up from s until we find an ancestor whose parent
       is `p`; that ancestor is a direct child of `p`. */
    for (auto & e : *paths) {
        auto cur = e;
        while (true) {
            auto parent = cur.parent();
            if (!parent)
                break;
            if (*parent == p) {
                if (auto bn = cur.baseName()) {
                    std::string name(*bn);
                    std::optional<Type> ty;
                    if (auto st = next->maybeLstat(cur))
                        ty = st->type;
                    else if (cur != e)
                        ty = SourceAccessor::tDirectory;
                    else
                        /* `cur == e`: a member of S that is a DIRECT child
                           of `p` and which `next` lacks. `maybeLstat(cur)`
                           returns nullopt for it (members are excluded from
                           `ancestorOfMember`, and `next` lacks it), so
                           listing it here would advertise a child that
                           cannot be stat'd or read — a readDirectory/lstat
                           divergence. Only synthesise INTERMEDIATE dirs
                           (`cur != e`), matching what `maybeLstat` returns.
                           (In the production `sourceViewSubset` flow every
                           member of S exists on `base`, so this is a
                           defensive guard against an over-approximated S.) */
                        break;
                    result.try_emplace(std::move(name), std::move(ty));
                }
                break;
            }
            if (parent->isRoot())
                break;
            cur = *parent;
        }
    }
    return result;
}

ref<SourceAccessor> makeDirectorySynthesizer(
    ref<SourceAccessor> base, std::shared_ptr<const std::set<CanonPath>> paths, MakeNotAllowedError makeNotAllowedError)
{
    return makePathSetOp<DirectorySynthesizerSourceAccessor>(
        std::move(base), std::move(paths), std::move(makeNotAllowedError));
}

} // namespace nix
