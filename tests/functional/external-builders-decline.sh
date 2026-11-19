#!/usr/bin/env bash

# An external builder may decline a build (exit 222). Nix then builds the
# derivation itself if it can, and build.json's `canBuildLocally` says in
# advance whether it will.

source common.sh

TODO_NixOS

needLocalStore "starts its own daemon"

expr="$TEST_ROOT/expr.nix"
cat > "$expr" <<EOF
{ name, system ? null, features ? [] }:
with import ${config_nix};
mkDerivation ({
  inherit name;
  requiredSystemFeatures = features;
  buildCommand = "echo local > \$out";
} // (if system == null then {} else { inherit system; }))
EOF

# Records `canBuildLocally` per derivation name, then declines.
external_builder="$TEST_ROOT/external-builder.sh"
cat > "$external_builder" <<EOF
#! $SHELL -e
PATH=${PATH@Q}
jq -r .canBuildLocally < "\$1" > "$TEST_ROOT/can-\$(jq -r .env.name < "\$1")"
printf "\2\n"
exit 222
EOF
chmod +x "$external_builder"

foreign=x68_46-xunil
builders="[{\"systems\": [\"$system\", \"$foreign\"], \"args\": [], \"program\": \"$external_builder\"}]"

# Build derivation NAME (further arguments go to `nix build`), check that
# build.json said EXPECTED, and that the fallback agreed: the build
# succeeds locally exactly when `canBuildLocally` is true.
check() {
    local name=$1 expected=$2
    shift 2
    local out
    if [[ $expected = true ]]; then
        out=$(nix build --no-link --print-out-paths --file "$expr" --argstr name "$name" "$@")
        [[ $(cat "$out") = local ]]
    else
        expectStderr 1 nix build --no-link --file "$expr" --argstr name "$name" "$@" \
            | grepQuiet "exit code 222"
    fi
    [[ $(cat "$TEST_ROOT/can-$name") = "$expected" ]]
}

ext=(--extra-experimental-features external-builders --external-builders "$builders")

# This system, nothing required: Nix builds it.
check local-ok true "${ext[@]}"

# A required system feature this store lacks: Nix does not.
check local-feature false "${ext[@]}" --arg features '["canlocal-missing-feature"]'

# A foreign system: Nix does not, unless it is an extra platform.
check local-foreign false "${ext[@]}" --argstr system "$foreign"
check local-foreign-extra true "${ext[@]}" --argstr system "$foreign" --extra-platforms "$foreign"

# Through the daemon, the trusted client's per-connection settings count.
cat >> "$test_nix_conf" <<EOF
extra-experimental-features = external-builders
external-builders = $builders
EOF
startDaemon

check daemon-ok true
check daemon-foreign false --argstr system "$foreign"
check daemon-foreign-extra true --argstr system "$foreign" --extra-platforms "$foreign"

# An external build takes a local build slot, so with max-jobs 0 the
# external builder never runs and never sees `canBuildLocally` false.
expectStderr 1 nix build --no-link --file "$expr" --argstr name daemon-no-jobs --max-jobs 0 \
    | grepQuiet "Unable to start any build"
[[ ! -e "$TEST_ROOT/can-daemon-no-jobs" ]]
