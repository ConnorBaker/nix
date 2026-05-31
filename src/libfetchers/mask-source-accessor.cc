#include "nix/fetchers/mask-source-accessor.hh"

namespace nix {

ref<SourceAccessor> makeMask(
    ref<SourceAccessor> base, std::shared_ptr<const std::set<CanonPath>> paths, MakeNotAllowedError makeNotAllowedError)
{
    return makePathSetOp<MaskSourceAccessor>(std::move(base), std::move(paths), std::move(makeNotAllowedError));
}

} // namespace nix
