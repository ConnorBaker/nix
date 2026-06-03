#!/usr/bin/env bash

# Increment 6 — negative control + referential integrity. The dual of
# source-copy-elision.sh: when the target is BUILT from source (not substituted),
# its deferred (fingerprintable) source MUST be copied into the store. The build
# materialises the `.drv`, and a `.drv` cannot register unless its `inputSrcs` are
# valid — so the deferred source copy thunk runs as part of the `.drv`'s
# referential closure. This proves elision is selective, not a blanket "never copy
# sources".

source ../common.sh

requireGit
enableFeatures "flakes"

clearStore

flake="$TEST_ROOT/flk"
createGitRepo "$flake"
mkdir -p "$flake/src"
echo "built-flake-source-payload" > "$flake/src/payload"
cat > "$flake/flake.nix" <<'EOF'
{
  outputs = { self }: let
    src = builtins.path { path = ./src; name = "flkbuiltmarker"; };
  in {
    packages.SYSTEM.default = derivation {
      name = "flakesrc-built-target";
      system = "SYSTEM";
      builder = "/bin/sh";
      # `${src}` as a trailing arg makes the source a (deferred) inputSrc without
      # the builder needing an external `cat`; `echo` is a shell builtin.
      args = [ "-c" "echo source-built-ok > $out" "${src}" ];
    };
  };
}
EOF
sed -i "s|SYSTEM|$system|g" "$flake/flake.nix"
git -C "$flake" add flake.nix src
git -C "$flake" commit -m init

flakeref="git+file://$flake#packages.$system.default"

# Build from source (no substituters) with lazy-derivations on.
out=$(nix build --no-link --print-out-paths --option eval-cache false \
    --option lazy-derivations true "$flakeref")

# The output is built and present, with the right contents.
nix-store --check-validity "$out" || fail "built output is missing"
[ "$(cat "$out")" = "source-built-ok" ] || fail "built output has wrong contents"

# ...and the source WAS copied into the store (referential integrity of the
# materialised `.drv`).
srcPath=$(ls -d "$NIX_STORE_DIR"/*flkbuiltmarker 2>/dev/null) \
    || fail "source 'flkbuiltmarker' was not copied into the store even though the target was built from source"

# The copied source is a valid, registered store object (the deferred copy went
# through `addToStore`), so it is referentially usable as the `.drv`'s `inputSrc`.
nix-store --check-validity "$srcPath" || fail "copied source is not a registered store path"

echo "source-copied-when-built: ok (source copied + registered)"
