#include <vector>

#include "nix/flake/settings.hh"
#include "nix/flake/flake-primops.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/unforce.hh"

namespace nix::flake {

Settings::Settings() {}

void Settings::configureEvalSettings(nix::EvalSettings & evalSettings) const
{
    for (auto & primOp : {primops::getFlake(*this), primops::parseFlakeRef, primops::flakeRefToString}) {
        /* Nix's own: not a plugin's (`unforceNativeCode`). */
        unforceNoteOwnExtraPrimOp(primOp);
        evalSettings.extraPrimOps.push_back(primOp);
    }
}

} // namespace nix::flake
