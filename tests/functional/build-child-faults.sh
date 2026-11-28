#!/usr/bin/env bash

source common.sh

# Local builds whose build child, or Nix around it, misbehaves at one point
# (see build-child-faults/shim.c).

needLocalStore "the build child is started by the process that builds"

export NIX_TEST_FAULT_DIR=$TEST_ROOT/faults

# Build $1 (a name) with the fault $NIX_TEST_FAULT injected, unsandboxed so
# that the builder can count its runs in $TEST_ROOT. No build here has a
# deadline: a Nix that never exits keeps the test waiting until the runner
# ends it. `meson test` does after its limit per test; `checkPhase`, in a
# package build or the development shell, has no limit. nix-build is then
# killed with this shell (--pdeathsig): a hung Nix may not act on SIGTERM.
withFault() {
    local name=$1
    shift
    rm -rf "$NIX_TEST_FAULT_DIR"
    mkdir -p "$NIX_TEST_FAULT_DIR"
    LD_PRELOAD=${_NIX_TEST_BUILD_DIR}/build-child-faults-shim.so setpriv --pdeathsig KILL \
        nix-build --no-out-link --option sandbox false "$@" -E "
        with import ${config_nix}; mkDerivation {
          name = \"$name\";
          buildCommand = \"echo run >> $TEST_ROOT/$name.runs; sleep 1; echo > \$out\";
        }"
}

# Nix's fds that the build child gets may have the numbers of the targets of
# others: here the pty slave, its stdout and stderr, is fd 0, the target of
# the build child's input.
NIX_TEST_FAULT=slave-on-stdin withFault slave-on-stdin < /dev/null
[[ -e $NIX_TEST_FAULT_DIR/slave-on-stdin ]] || fail "the fault was not injected"
[[ $(< "$TEST_ROOT/slave-on-stdin.runs") = run ]]

# Nix stops a build child that failed to set up, also before it left Nix's
# process group (with setsid()).
status=0
NIX_TEST_FAULT=child-stops-before-setsid withFault child-stops-before-setsid 2> "$TEST_ROOT/log" || status=$?
[[ $status != 0 ]]
grepQuiet "unexpected error type 'Fail'" "$TEST_ROOT/log"
[[ -e $NIX_TEST_FAULT_DIR/child-stopped ]] || fail "the fault was not injected"
[[ ! -e $TEST_ROOT/child-stops-before-setsid.runs ]]

# The build child server exits before it replied with its version, as one
# that cannot start would.
status=0
NIX_TEST_FAULT=server-exits-at-start withFault server-exits-at-start 2> "$TEST_ROOT/log" || status=$?
[[ $status != 0 ]]
grepQuiet "exited before it replied with its version" "$TEST_ROOT/log"
[[ -e $NIX_TEST_FAULT_DIR/server-exits-at-start ]] || fail "the fault was not injected"

# The build child server's stdout is not Nix's, which in a daemon is the
# client's connection.
NIX_TEST_FAULT=server-stdout withFault server-stdout
[[ -e $NIX_TEST_FAULT_DIR/server-stdout-null ]] || fail "the build child server's stdout is not /dev/null"

# The build child server exits after it started the build child, before it
# replied: Nix starts it again through another server, and the first one
# does not build.
NIX_TEST_FAULT=server-exits-before-reply withFault server-exits-before-reply
[[ -e $NIX_TEST_FAULT_DIR/server-exits-before-reply ]] || fail "the fault was not injected"
[[ $(< "$TEST_ROOT/server-exits-before-reply.runs") = run ]] || fail "the build ran $(wc -l < "$TEST_ROOT/server-exits-before-reply.runs") times"

# That first build child is Nix's child (CLONE_PARENT), although Nix never
# got its pid: Nix reaps it, rather than leaving it a zombie. The builder
# counts the zombie children of the nix-build that runs it.
cat > "$TEST_ROOT/count-zombies.sh" << 'EOF'
sleep 1
nix=$PPID
for _ in 1 2 3 4 5; do
    [[ $nix -gt 1 && $(< "/proc/$nix/comm") != nix-build ]] || break
    stat=$(< "/proc/$nix/stat")
    read -r -a fields <<< "${stat##*") "}"
    nix=${fields[1]}
done
if [[ $nix -le 1 || $(< "/proc/$nix/comm") != nix-build ]]; then
    echo "no nix-build among the builder's ancestors" > "$1"
    return
fi
zombies=0
for stat in /proc/[0-9]*/stat; do
    { line=$(< "$stat"); } 2> /dev/null || continue
    read -r -a fields <<< "${line##*") "}"
    if [[ ${fields[0]-} == Z && ${fields[1]-} == "$nix" ]]; then
        zombies=$((zombies + 1))
    fi
done
echo "$zombies" > "$1"
EOF
rm -rf "$NIX_TEST_FAULT_DIR"
mkdir -p "$NIX_TEST_FAULT_DIR"
NIX_TEST_FAULT=server-exits-before-reply LD_PRELOAD=${_NIX_TEST_BUILD_DIR}/build-child-faults-shim.so setpriv --pdeathsig KILL \
    nix-build --no-out-link --option sandbox false -E "
    with import ${config_nix}; mkDerivation {
      name = \"unreported-child\";
      buildCommand = \"source $TEST_ROOT/count-zombies.sh $TEST_ROOT/zombies; echo > \$out\";
    }"
[[ -e $NIX_TEST_FAULT_DIR/server-exits-before-reply ]] || fail "the fault was not injected"
[[ $(< "$TEST_ROOT/zombies") = 0 ]] || fail "zombie children of Nix: $(< "$TEST_ROOT/zombies")"

# A malformed request gets an error as its reply.
status=0
NIX_TEST_FAULT=malformed-request withFault malformed-request 2> "$TEST_ROOT/log" || status=$?
[[ $status != 0 ]] || fail "a malformed request started a build"
grepQuiet "starting the build child: Invalid argument" "$TEST_ROOT/log"
[[ -e $NIX_TEST_FAULT_DIR/malformed-request ]] || fail "the fault was not injected"

# Nix's own error in sending a request or in receiving the reply fails
# the build: it is not taken for a server that went away, which Nix would
# kill and start again.
expectOwnError() {
    local status=0
    NIX_TEST_FAULT=$1 withFault "$1" 2> "$TEST_ROOT/log" || status=$?
    [[ $status != 0 ]] || fail "the build succeeded after $1"
    [[ -e $NIX_TEST_FAULT_DIR/$1 ]] || fail "the fault was not injected"
    grepQuiet "$2: Bad file descriptor" "$TEST_ROOT/log"
}
expectOwnError request-fails sendmsg
expectOwnError reply-fails "receiving from the build child server"

# Nix stops waiting when interrupted, where the fault $1 makes it wait for
# the process that the file $2 names.
expectInterruptible() {
    rm -rf "$NIX_TEST_FAULT_DIR"
    mkdir -p "$NIX_TEST_FAULT_DIR"
    NIX_TEST_FAULT=$1 LD_PRELOAD=${_NIX_TEST_BUILD_DIR}/build-child-faults-shim.so setpriv --pdeathsig KILL \
        nix-build --no-out-link --option sandbox false -E "
        with import ${config_nix}; mkDerivation { name = \"$1\"; buildCommand = \"echo > \$out\"; }" \
        2> "$TEST_ROOT/log" &
    local nixPid=$! status=0 waitedFor
    until [[ -e $NIX_TEST_FAULT_DIR/$2 ]] || ! kill -0 "$nixPid" 2> /dev/null; do sleep 0.1; done
    [[ -e $NIX_TEST_FAULT_DIR/$2 ]] || fail "the fault was not injected"
    waitedFor=$(< "$NIX_TEST_FAULT_DIR/$2")
    kill -INT "$nixPid"
    wait "$nixPid" || status=$?
    if [[ $(tr '\0' ' ' < "/proc/$waitedFor/cmdline" 2> /dev/null) = __build-child* ]]; then kill -9 "$waitedFor" 2> /dev/null || true; fi
    [[ $status = 1 ]]
}
# For the build child server's reply,
expectInterruptible server-hangs-before-reply server-hanging
# and for a build child that the server started but did not report.
expectInterruptible unreported-child-hangs child-hanging

# Also for that reply when the interrupt comes earlier, after Nix's request
# and before Nix waits: there Nix interrupts itself, and gets no reply.
status=0
NIX_TEST_FAULT=interrupted-after-request withFault interrupted-after-request 2> "$TEST_ROOT/log" || status=$?
[[ $status = 1 ]]
grepQuiet "interrupted by the user" "$TEST_ROOT/log"
