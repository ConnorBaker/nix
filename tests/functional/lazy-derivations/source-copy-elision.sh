#!/usr/bin/env bash

# Increment 6 — source-copy elision (LD-S2 extended from `.drv`s to their
# sources). When a build target's output is substitutable, not only is its
# `.drv` elided, the COPY of its (content-fingerprintable) source inputs into
# the store is elided too.
#
# Mechanism: under `lazy-derivations`, `derivationStrict` names each SourceVirtual
# input by its content-addressed path (the unavoidable `narHashOf` hash walk) but
# defers the copy as a thunk on the asyncPathWriter. The deferred source rides the
# deferred `.drv`'s referential closure — it is copied iff the `.drv` is
# materialised. A substitutable target never materialises its `.drv`, so the copy
# thunk never fires and the source is dropped unwritten when the queue is
# destroyed.
#
# A *fingerprintable* source is required to reach the deferred SourceVirtual path
# (a plain local dir is copied eagerly by `builtins.path` itself); a flake-relative
# `builtins.path` over a git-fetched flake source is the canonical case (the CI
# workload Increment 6 targets). `eval-cache` is disabled so `derivationStrict`
# actually re-runs on the substituting build (otherwise the cached drvPath would
# make the test vacuous).
#
# Non-vacuity: source-copied-when-built.sh builds the SAME source and DOES copy it.

source ../common.sh

requireGit
needLocalStore "“--no-require-sigs” can’t be used with the daemon"
enableFeatures "flakes"

clearStore
clearCache

cacheDir="$TEST_ROOT/cache"
flake="$TEST_ROOT/flk"
createGitRepo "$flake"
mkdir -p "$flake/src"
echo "flake-source-payload" > "$flake/src/payload"
cat > "$flake/flake.nix" <<'EOF'
{
  outputs = { self }: let
    src = builtins.path { path = ./src; name = "flkmarker"; };
  in {
    packages.SYSTEM.default = derivation {
      name = "flakesrc-target";
      system = "SYSTEM";
      builder = "/bin/sh";
      # `${src}` as a trailing arg makes the source a (deferred) inputSrc without
      # the builder needing an external `cat`; `echo` is a shell builtin.
      args = [ "-c" "echo elision-built > $out" "${src}" ];
    };
  };
}
EOF
sed -i "s|SYSTEM|$system|g" "$flake/flake.nix"
git -C "$flake" add flake.nix src
git -C "$flake" commit -m init

flakeref="git+file://$flake#packages.$system.default"

# 1. Build from source; push the OUTPUT (+ closure) to a binary cache. The source
#    is NOT a runtime dependency of the output (the build cats its contents), so
#    the cached output closure does not drag the source along.
out=$(nix build --no-link --print-out-paths --option eval-cache false "$flakeref")
nix copy --to "file://$cacheDir" "$out"

clearStore

# 2. Build from the cache with lazy-derivations on: the output substitutes.
#    `-j0` forbids local building, so the output MUST come from the substituter.
nix build --no-link --option eval-cache false --option lazy-derivations true \
    --substitute --substituters "file://$cacheDir" --no-require-sigs -j0 "$flakeref"

# 3. The output is present (substituted)...
nix-store --check-validity "$out" || fail "substituted output is missing"

# ...but the source was elided — never copied into the store (the Increment 6 win).
if ls "$NIX_STORE_DIR"/*flkmarker 2>/dev/null; then
    fail "source 'flkmarker' was copied into the store even though the target substituted (no source elision)"
fi

echo "source-copy-elision: ok (source elided)"
