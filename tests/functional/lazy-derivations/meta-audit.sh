#!/usr/bin/env bash

# LD-V3 (meta) — the build path reads `.drv` BYTES (readDerivation), never the
# recursive modulo-hash, so a cold `drvHashes` memo after a warm eval-cache hit
# cannot fail a deferred read. Encoded as a source audit: no
# `hashDerivationModulo`/`pathDerivationModulo` caller exists under
# src/libstore/build/. If one is added, this fails, forcing a re-think of the
# §5 soundness argument.

source ../common.sh

# Locate the source tree (skip gracefully if unavailable in this run).
root="${_NIX_TEST_SOURCE_DIR:-}/../.."
buildDir="$root/src/libstore/build"
if [ ! -d "$buildDir" ]; then
    echo "meta-audit: SKIP (source tree not available)"
    exit 0
fi

if grep -rn "hashDerivationModulo\|pathDerivationModulo" "$buildDir" 2>/dev/null; then
    fail "LD-V3: a modulo-hash caller appeared under src/libstore/build/ — the §5 soundness argument (build reads .drv bytes, not the modulo hash) no longer holds"
fi

# LD-S4 (meta) — the build goals read `.drv`s only through the Store interface
# (`Store::readDerivation`/`readInvalidDerivation`), which `LazyDrvStore`
# overrides — so a deferred `.drv` is served to EVERY build-side reader. A goal
# that parsed a `.drv` from raw bytes would bypass the overlay; assert none does,
# and (non-vacuity) that the build path does read `.drv`s via the interface.
if grep -rn "parseDerivation" "$buildDir" 2>/dev/null; then
    fail "LD-S4: src/libstore/build/ parses a .drv directly, bypassing Store::readDerivation / the LazyDrvStore overlay — a deferred .drv would not be served"
fi
grep -rq "readDerivation" "$buildDir" \
    || fail "LD-S4: expected build/ to read .drvs via Store::readDerivation (audit gone vacuous?)"

# LD-X4 (meta) — every eval-time `.drv`-byte consumer drains the queue before
# reading. The guarded sites are a finite, enumerable set; assert each still
# contains a flush/overlay call, so a future edit that drops a guard trips here.
exprDir="$root/src/libexpr"
check_guard() {  # $1 = file, $2 = description
    [ -f "$1" ] || return 0
    grep -Eq "waitForAllPaths|waitForPath|isPending|materialiseDeferred" "$1" \
        || fail "LD-X4: the .drv-byte flush guard disappeared from $(basename "$1") ($2)"
}
check_guard "$exprDir/eval-cache.cc" "forceDerivation warm-eval-cache tolerance (C2)"
check_guard "$exprDir/primops.cc"    "realiseContext / DrvDeep"
check_guard "$exprDir/paths.cc"      "ensureLazyPathsCopied value-output drain"

echo "meta-audit: ok (LD-V3 no modulo-hash in build/; LD-S4 build reads .drvs via the overlay; LD-X4 eval-side flush guards present)"
