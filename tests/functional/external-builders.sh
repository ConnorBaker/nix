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
