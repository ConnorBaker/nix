#include "nix/fetchers/soundness-guard-source-accessor.hh"

namespace nix {

SoundnessGuardSourceAccessor::~SoundnessGuardSourceAccessor() = default;

ref<SourceAccessor> makeSoundnessGuard(
    ref<SourceAccessor> base, std::shared_ptr<const std::set<CanonPath>> paths, MakeNotAllowedError makeNotAllowedError)
{
    return makePathSetOp<SoundnessGuardSourceAccessor>(
        std::move(base), std::move(paths), std::move(makeNotAllowedError));
}

} // namespace nix
