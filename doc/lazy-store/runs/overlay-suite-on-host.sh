#!/usr/bin/env bash
# The local-overlay-store suite needs an overlayfs mount inside a user namespace, which the Nix build sandbox
# does not allow (05-validation.md section 7).  This script ships a revision of this repository to a Linux host
# that allows unprivileged user namespaces and starts, detached there, a per-file build of the tree and
# `meson test --suite local-overlay-store`; the per-test lines land in ~/<dir>/overlay-run.log on the host.
# Usage: overlay-suite-on-host.sh <ssh host> [<revision, default HEAD>] [<directory on the host, default nix-lazy>]
# On Ubuntu the host needs `sysctl kernel.apparmor_restrict_unprivileged_userns=0` (root; not persisted).
# The suite's own probe (tests/functional/common/vars.sh: `unshare --user --mount-proc true`) requires mounting a
# fresh procfs inside a user namespace, which the kernel refuses on hosts whose /proc is not fully visible; the
# overlay tests themselves need only `unshare --mount --map-root-user` and `mount -t overlay`, so the job asserts
# the environment the probe would set (`_canUseSandbox=1`) after checking that the mounts the tests make work.
set -euo pipefail
HOST=${1:?ssh host}
REPO=$(git rev-parse --show-toplevel)
TIP=$(git -C "$REPO" rev-parse "${2:-HEAD}")
DIR=${3:-nix-lazy}
SRC=$(mktemp -d /tmp/nix-overlay-src.XXXXXX)
git -C "$REPO" archive --format=tar "$TIP" | tar -x -C "$SRC"
echo "$TIP" > "$SRC/.branch-tip"
rsync -a --delete "$SRC"/ "$HOST:$DIR/"
rm -rf "$SRC"
ssh "$HOST" 'cd '"$DIR"' && cat > overlay-job.sh <<'"'"'JOB'"'"'
set -u
cd ~/'"$DIR"'
echo "== tip $(cat .branch-tip) on $(uname -a)"
echo "== userns: $(unshare --mount --map-root-user true && echo ok || echo BLOCKED)"
nix develop -c sh -c "meson setup build -Dpkg_config_path=\"\$PKG_CONFIG_PATH\"" ; echo "setup exit=$?"
nix develop -c ninja -C build ; echo "build exit=$?"
unshare --mount --map-root-user sh -c "mount -t tmpfs none /mnt && mkdir -p /mnt/l /mnt/u /mnt/w /mnt/m && mount -t overlay overlay -o lowerdir=/mnt/l,upperdir=/mnt/u,workdir=/mnt/w /mnt/m" ; echo "overlay mount in a user namespace exit=$?"
_canUseSandbox=1 nix develop -c meson test -C build --no-rebuild --suite local-overlay-store ; echo "overlay suite exit=$?"
echo "== per test:"; grep -E " (OK|SKIP|FAIL|TIMEOUT) " build/meson-logs/testlog.txt | sed "s/.*nix-functional-tests://" | awk "{print \$1, \$2}"
echo "== DONE"
JOB
nohup bash overlay-job.sh > overlay-run.log 2>&1 &
echo "started; log: ~/'"$DIR"'/overlay-run.log"'
