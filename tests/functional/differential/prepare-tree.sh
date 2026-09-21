#!/usr/bin/env bash
#
# Prepare a runnable copy of a functional test tree, as meson would, plus the
# harness hook.
#
# Usage: prepare-tree.sh <tests/functional dir> <dest> <nix bindir> <coreutils bindir> <bash> <version> <system> [tools PATH]
#
# Produces <dest>/tests/functional (the copy, with common/functions.sh patched so
# that doClearStore snapshots the store before clearing when HARNESS_OUT is set),
# <dest>/scripts/nix-profile.sh.in (bash-profile.sh reads ../../scripts/ relative to
# the tests directory, so the repository layout is kept),
# <dest>/build/{common/subst-vars.sh,config.nix} (what tests/functional/meson.build
# and common/meson.build generate with configure_file), and <dest>/modes/*.env,
# the mode files made from modes/*.env.in beside this script: @tree@ is <dest>,
# @nix@ the package whose bin/ is <nix bindir>, @bash@ the bash given, @tools@
# the optional tools PATH (default: the bash's directory, then <coreutils bindir>),
# and @branchNix@ the store path in BRANCH_NIX when set.  <dest> must be a
# resolved path (`/private/tmp`, not `/tmp`, on macOS: the suite's
# characterisation tests normalise the working directory).
set -euo pipefail
src=$1 dest=$2 bindir=$3 coreutils=$4 bashpath=$5 version=$6 system=$7
tools=${8:-$(dirname "$bashpath"):$coreutils}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

rm -rf "$dest"
mkdir -p "$dest/build/common" "$dest/tests" "$dest/scripts" "$dest/modes"
dest=$(cd "$dest" && pwd -P)
cp -R "$src" "$dest/tests/functional"
cp "$src/../../scripts/nix-profile.sh.in" "$dest/scripts/"
cp "$src/../../.version" "$dest/"

subst() {
    sed -e "s|@bash@|$bashpath|g" -e "s|@bindir@|$bindir|g" -e "s|@coreutils@|$coreutils|g" \
        -e "s|@dot@||g" -e "s|@sandbox_shell@||g" -e "s|@PACKAGE_VERSION@|$version|g" -e "s|@system@|$system|g" "$1"
}
subst "$src/common/subst-vars.sh.in" > "$dest/build/common/subst-vars.sh"
subst "$src/config.nix.in" > "$dest/build/config.nix"

python3 - "$dest/tests/functional/common/functions.sh" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
s = p.read_text()
old = 'doClearStore() {\n    echo "clearing store..."\n'
new = ('doClearStore() {\n'
       '    if [[ -n "${HARNESS_OUT-}" ]]; then\n'
       '        # differential harness: record the store before it is cleared\n'
       '        source "${HARNESS_DIR:?}/lib.sh"\n'
       '        HARNESS_EPOCH=$(( ${HARNESS_EPOCH:-0} + 1 ))\n'
       '        harness_snapshot "clear-$HARNESS_EPOCH" || true\n'
       '    fi\n'
       '    echo "clearing store..."\n')
assert s.count(old) == 1, "doClearStore not found in the expected form; adjust prepare-tree.sh"
p.write_text(s.replace(old, new))
PY
nixpkg=$(dirname "$bindir")
for template in "$here"/modes/*.env.in; do
    mode=$(basename "${template%.in}")
    sed -e "s|@tree@|$dest|g" -e "s|@nix@|$nixpkg|g" -e "s|@bash@|$bashpath|g" -e "s|@tools@|$tools|g" \
        -e "s|@branchNix@|${BRANCH_NIX:-@branchNix@}|g" "$template" > "$dest/modes/$mode"
done
echo "prepared $dest (bindir=$bindir coreutils=$coreutils bash=$bashpath version=$version system=$system; modes: $(ls "$dest/modes" | tr '\n' ' '))"
