#include "nix/fetchers/restrict-source-accessor.hh"

namespace nix {

ref<SourceAccessor> makeRestrict(
    ref<SourceAccessor> base, std::shared_ptr<const std::set<CanonPath>> paths, MakeNotAllowedError makeNotAllowedError)
{
    return makePathSetOp<RestrictSourceAccessor>(std::move(base), std::move(paths), std::move(makeNotAllowedError));
}

} // namespace nix
