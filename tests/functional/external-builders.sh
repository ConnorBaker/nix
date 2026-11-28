#!/usr/bin/env bash

source common.sh

TODO_NixOS

needLocalStore "'--external-builders' can’t be used with the daemon"

expr="$TEST_ROOT/expr.nix"
cat > "$expr" <<EOF
with import ${config_nix};
mkDerivation {
  name = "external";
  system = "x68_46-xunil";
  buildCommand = ''
    echo xyzzy
    printf foo > \$out
  '';
}
EOF

external_builder="$TEST_ROOT/external-builder.sh"
cat > "$external_builder" <<EOF
#! $SHELL -e

PATH=${PATH@Q}

[[ "\$1" = bla ]]

system="\$(jq -r .system < "\$2")"
builder="\$(jq -r .builder < "\$2")"
args="\$(jq -r '.args | join(" ")' < "\$2")"
export buildCommand="\$(jq -r .env.buildCommand < "\$2")"
export out="\$(jq -r .env.out < "\$2")"
[[ \$system = x68_46-xunil ]]

printf "\2\n"

# In a real external builder, we would now call something like qemu to emulate the system.
"\$builder" \$args

printf bar >> \$out
EOF
chmod +x "$external_builder"

nix build -L --file "$expr" --out-link "$TEST_ROOT/result" \
  --extra-experimental-features external-builders \
  --external-builders "[{\"systems\": [\"x68_46-xunil\"], \"args\": [\"bla\"], \"program\": \"$external_builder\"}]"

[[ $(cat "$TEST_ROOT/result") = foobar ]]

# A builder that is still starting must not hold up the others: here
# neither of the first two builds' external builders finishes starting
# (prints \2) before the other has started too. Opening a FIFO blocks
# until its other end is opened, so this deadlocks if the builders are
# started one by one. `meson test` then ends it at its limit per test; a
# package build runs the suite without that limit, and only the outer
# Nix's `timeout` or `max-silent-time` end it.
cat > "$expr" <<EOF
with import ${config_nix};
map (name: mkDerivation {
  inherit name;
  system = "x68_46-xunil";
  buildCommand = "printf foo > \$out";
}) [ "a-waits" "b-other" "c-third" ]
EOF

cat > "$external_builder" <<EOF
#! $SHELL -e

PATH=${PATH@Q}

out="\$(jq -r .env.out < "\$2")"
if [[ \$out = *-a-waits ]]; then
  : < "$TEST_ROOT/both-started"
elif [[ \$out = *-b-other ]]; then
  : > "$TEST_ROOT/both-started"
fi

cat <<< \$'\2\nfirst line' # one write
printf foo > \$out
EOF

mkfifo "$TEST_ROOT/both-started"
nix build -L --file "$expr" --no-link --max-jobs 2 -vvvv \
  --extra-experimental-features external-builders \
  --external-builders "[{\"systems\": [\"x68_46-xunil\"], \"args\": [\"bla\"], \"program\": \"$external_builder\"}]" \
  2> "$TEST_ROOT/log"
# A starting builder holds its slot; what it writes with \2 is build log.
grepQuiet "c-third.drv': wait for build slot" "$TEST_ROOT/log"
grepQuiet "a-waits> first line" "$TEST_ROOT/log"

# Failures while a builder starts are still reported, whether it sends
# an error (here: it cannot be executed) or exits before reporting \2.
cat > "$expr" <<EOF
with import ${config_nix};
mkDerivation {
  name = "start-failure";
  system = "x68_46-xunil";
  buildCommand = "printf foo > \$out";
}
EOF
startFailure() {
  expectStderr 1 nix build --file "$expr" --no-link --max-silent-time 1 \
    --extra-experimental-features external-builders \
    --external-builders "[{\"systems\": [\"x68_46-xunil\"], \"args\": [], \"program\": \"$1\"}]"
}
startFailure "$TEST_ROOT/no-such-builder" | grepQuiet "while setting up the build environment"
# An error longer than one read of the builder's output.
startFailure "$TEST_ROOT$(printf '/%0200d' $(seq 25))" | grepQuiet '/0\{198\}25": '
# A builder that exits before reporting \2, after more than
# `max-silent-time`: while it is starting, it is not timed out and the
# build is not shown as started.
cat > "$external_builder" <<EOF
#! $SHELL
PATH=${PATH@Q}
sleep 2
exit 1
EOF
startFailure "$external_builder" > "$TEST_ROOT/log"
grepQuiet "while waiting for the build environment for .* to initialize" "$TEST_ROOT/log"
grepQuietInverse "building '" "$TEST_ROOT/log"

# A build that is cancelled is killed, also when its child was started
# only just before and has no process group of its own yet. Whether the
# kill meets such a child is a matter of scheduling: keep Nix and its
# children on one CPU, and try a few times. So this can miss a kill that
# fails there. It cannot fail when the kill works: Nix does not sleep
# before the kill (its messages go to /dev/null), and after it only a
# builder that it missed still runs, to see Nix asleep or gone.
if [[ -e /proc/self/stat ]] && type -P taskset > /dev/null; then
  cat > "$expr" <<EOF
with import ${config_nix};
{
  a-cancelled = mkDerivation {
    name = "a-cancelled";
    buildCommand = "until ! read -r _ _ state _ < /proc/\$PPID/stat || [[ \$state = S ]]; do :; done; touch $TEST_ROOT/survived";
  };
  b-rejected = mkDerivation { name = "b-rejected"; system = "no-such-system"; buildCommand = "true"; };
}
EOF
  cpu=$(taskset -cp $$ | sed -E 's/.*: *([0-9]+).*/\1/')
  for _ in {1..5}; do
    expect 100 taskset -c "$cpu" nix-build "$expr" --no-out-link --max-jobs 2 --option build-hook '' 2> /dev/null
    [[ ! -e $TEST_ROOT/survived ]]
  done
fi
