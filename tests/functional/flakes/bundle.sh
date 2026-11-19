#!/usr/bin/env bash

source common.sh

cp ../simple.nix ../simple.builder.sh "${config_nix}" "$TEST_HOME"

cd "$TEST_HOME"

cat <<EOF > flake.nix
{
    outputs = {self}: {
      bundlers.$system = rec {
        simple = drv:
          if drv?type && drv.type == "derivation"
          then drv
          else self.packages.$system.default;
        default = simple;
      };
      packages.$system.default = import ./simple.nix;
      apps.$system.default = {
        type = "app";
        program = "\${import ./simple.nix}/hello";
      };
    };
}
EOF

nix build .#
nix bundle --bundler .# .#
nix bundle --bundler .#bundlers."$system".default .#packages."$system".default
nix bundle --bundler .#bundlers."$system".simple  .#packages."$system".default

nix bundle --bundler .#bundlers."$system".default .#apps."$system".default
nix bundle --bundler .#bundlers."$system".simple  .#apps."$system".default

# Function auto call
cat <<'EOF' > function.nix
{ x ? 1 }:
with import ./config.nix;
mkDerivation {
  name = "bundle-function";
  buildCommand = "echo hi > $out";
}
EOF
# On NixOS, config.nix brings paths of the system store into context. No
# substituter has them, so evaluation copies them from the build store
# into the separate evaluation store. They are unsigned, so that store
# must not require signatures (as in eval-store.sh).
nix bundle \
    --eval-store "$TEST_ROOT/bundle-eval-store?require-sigs=false" \
    --bundler .#bundlers."$system".simple \
    -f function.nix \
    -o function-bundle
[[ $(cat function-bundle) = hi ]]
