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

drvPath="$(nix-instantiate "$expr")"

external_builder="$TEST_ROOT/external-builder.sh"
cat > "$external_builder" <<EOF
#! $SHELL -e

PATH=${PATH@Q}

[[ "\$1" = bla ]]
[[ \$(pwd -P) = \$(realpath "\$(jq -r .tmpDir < "\$2")") ]]

system="\$(jq -r .system < "\$2")"
builder="\$(jq -r .builder < "\$2")"
args="\$(jq -r '.args | join(" ")' < "\$2")"
export buildCommand="\$(jq -r .env.buildCommand < "\$2")"
export out="\$(jq -r .env.out < "\$2")"
[[ \$system = x68_46-xunil ]]
[[ "\$(jq -r .drvPath < "\$2")" = ${drvPath@Q} ]]

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
# an error or exits before reporting \2.
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
startFailure "$TEST_ROOT/no-such-builder" | grepQuiet 'could not start program ".*/no-such-builder"'
# Nix reports a failure to execute an external builder itself, not
# through the builder's output. So this builder sends an error the way
# Nix's own build child does: \1, then the error's type, level, name,
# message, position and traces. The message is longer than one read of
# the builder's output.
cat > "$TEST_ROOT/sends-error.sh" <<EOF
#! $SHELL
printf '\1\n\5\0\0\0\0\0\0\0Error\0\0\0'
printf '\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0'
printf '\x88\x13\0\0\0\0\0\0%05000d' 0
printf '\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0'
exit 1
EOF
chmod +x "$TEST_ROOT/sends-error.sh"
startFailure "$TEST_ROOT/sends-error.sh" > "$TEST_ROOT/sends-error.log"
grepQuiet "while setting up the build environment" "$TEST_ROOT/sends-error.log"
grepQuiet "error: 0\{5000\}$" "$TEST_ROOT/sends-error.log"
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

# An external builder that closes its output and keeps working is waited
# for, like any other builder (see builder-exit.sh).
expr="$TEST_ROOT/closes-output.nix"
cat > "$expr" <<EOF
with import ${config_nix};
mkDerivation {
  name = "external-closes-output";
  system = "x68_46-xunil";
  buildCommand = "";
}
EOF

external_builder="$TEST_ROOT/external-builder-closes-output.sh"
cat > "$external_builder" <<EOF
#! $SHELL -e

PATH=${PATH@Q}

out="\$(jq -r .env.out < "\$1")"

printf "\2\n"
exec >&- 2>&-
sleep 1
printf foo > \$out
EOF
chmod +x "$external_builder"

buildClosesOutput() {
  nix build -L --file "$expr" --no-link --print-out-paths --timeout 120 \
    --extra-experimental-features external-builders \
    --external-builders "[{\"systems\": [\"x68_46-xunil\"], \"args\": [], \"program\": \"$external_builder\"}]"
}

if canWaitForBuilderExit; then
  [[ $(cat "$(buildClosesOutput)") = foo ]]
else
  expectStderr 1 buildClosesOutput | grepQuiet "builder failed due to signal"
fi

# Where nothing can be built, with `max-jobs = 0` and no build hook, the
# output locks are still taken, once, without and with an external
# builder: taking them is what waits for another process's build.
lockRounds() {
  expectStderr 1 nix build --impure --no-link --max-jobs 0 --option build-hook '' -vvvv "$@" --expr "
    with import ${config_nix};
    mkDerivation { name = \"unbuildable\"; system = \"x68_46-xunil\"; buildCommand = \"\"; }
  " | sed -n "/: trying to build/p" | tee "$TEST_ROOT/rounds"
  [[ $(wc -l < "$TEST_ROOT/rounds") -eq 1 ]]
}
lockRounds
lockRounds --extra-experimental-features external-builders \
  --external-builders "[{\"systems\": [\"x68_46-xunil\"], \"args\": [\"bla\"], \"program\": \"$external_builder\"}]"
