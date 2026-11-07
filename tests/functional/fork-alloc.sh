#!/usr/bin/env bash

source common.sh

# A child that Nix forks from its multithreaded process must not allocate
# before it execs: another thread may have held the allocator's lock at the
# fork, and the child would wait for it forever. The shim reports any
# allocator call in such a child to fd 9.

needLocalStore "the children are forked by the process that builds"

log=$TEST_ROOT/fork-alloc.log

detect() {
    : > "$log"
    LD_PRELOAD=${_NIX_TEST_BUILD_DIR}/fork-alloc-shim.so "$@" 9>> "$log"
}

expectNoChildAlloc() {
    detect "$@"
    [[ ! -s $log ]] || { cat "$log" >&2; fail "a forked child allocated before exec: $*"; }
}

# The control: the shim sees a forked child that allocates.
detect bash -c ": \$(:)"
grepQuiet "allocator call in a forked child" "$log"

# The long socket path helper: this basename is too long for sun_path even
# as /proc/self/fd/<N>/<base>.
mkdir -p "$TEST_ROOT/sock"
detect nix store info --store "unix://$TEST_ROOT/sock/$(printf 's%.0s' {1..96})" || true
[[ ! -s $log ]] || { cat "$log" >&2; fail "the socket helper's child allocated"; }

# The external builder.
cat > "$TEST_ROOT/fork-alloc.nix" <<EOF2
with import ${config_nix};
mkDerivation { name = "fork-alloc"; system = "x68_46-xunil"; buildCommand = "true"; }
EOF2
cat > "$TEST_ROOT/fork-alloc-builder.sh" <<EOF2
#! $SHELL -e
printf '\2\n'
printf foo > "\$(PATH=${PATH@Q} jq -r .env.out < "\$1")"
EOF2
chmod +x "$TEST_ROOT/fork-alloc-builder.sh"
expectNoChildAlloc nix build --no-link -f "$TEST_ROOT/fork-alloc.nix" \
    --extra-experimental-features external-builders \
    --external-builders "[{\"systems\": [\"x68_46-xunil\"], \"program\": \"$TEST_ROOT/fork-alloc-builder.sh\", \"args\": []}]"

# The pager, which needs a terminal: the shim goes to the command in it.
inTerminal() {
    : > "$log"
    script -qec "LD_PRELOAD=${_NIX_TEST_BUILD_DIR}/fork-alloc-shim.so $*" /dev/null 9>> "$log"
}
inTerminal bash -c "': \$(:)'"
grepQuiet "allocator call in a forked child" "$log"
out=$(inTerminal TERM=xterm "NIX_PAGER='cat; echo PAGER-RAN'" nix-store -q)
[[ $out = *PAGER-RAN* && ! -s $log ]] || { cat "$log" >&2; fail "the pager's child allocated or did not run"; }
