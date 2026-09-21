#!/usr/bin/env bash
# run-mutation.sh N...: for each row N of mutations.py (05-validation.md section 4): apply the mutation to the tree,
# record the patch, build the row's targets, run its tests (quoting the discriminating lines), reverse the patch,
# touch the files, rebuild, and check the tree is clean.  Run inside `nix develop` from a worktree with a `build/`
# directory: LAZY_STORE_TREE=<worktree> MUTATION_OUT=<dir> run-mutation.sh 1 2 3
set -u
W=${LAZY_STORE_TREE:-$PWD}
O=${MUTATION_OUT:-$W/../mutation-out}; mkdir -p "$O"
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export NIX_REFERENCE_BIN=${NIX_REFERENCE_BIN:?set NIX_REFERENCE_BIN to a bin directory of a Nix built from master}
cd "$W" || exit 1
run_unit() { # lib filter log
  local data=$W/src/lib$1-tests/data; [ -d "$data" ] || data=
  _NIX_TEST_UNIT_DATA=$data "build/src/lib$1-tests/nix-$1-tests" --gtest_filter="$2" > "$3" 2>&1; echo $?
}
for N in "$@"; do
  spec=$(LAZY_STORE_TREE=$W python3 "$HERE/mutations.py" apply "$N") || { echo "M$N: APPLY FAILED"; exit 1; }
  title=$(echo "$spec" | python3 -c 'import json,sys; print(json.load(sys.stdin)["title"])')
  files=$(echo "$spec" | python3 -c 'import json,sys; print(" ".join(json.load(sys.stdin)["files"]))')
  targets=$(echo "$spec" | python3 -c 'import json,sys; print(" ".join(json.load(sys.stdin)["targets"]))')
  expectfail=$(echo "$spec" | python3 -c 'import json,sys; print(json.load(sys.stdin)["expect_build_failure"])')
  echo "=== M$N: $title"
  echo "    files: $files"
  git diff > "$O/M$N.patch"
  if [ -n "$targets" ]; then
    # shellcheck disable=SC2086
    ninja -C build $targets > "$O/M$N.build.log" 2>&1; bexit=$?
    echo "    build exit $bexit (expect failure: $expectfail)"
  else bexit=0; fi
  if [ "$expectfail" != True ] && [ $bexit -ne 0 ]; then grep -n "error:" "$O/M$N.build.log" | sed 's/^/    | /'; fi
  if [ $bexit -eq 0 ] || [ "$expectfail" = True ]; then
    k=0
    echo "$spec" | python3 -c 'import json,sys; [print("\t".join(t)) for t in json.load(sys.stdin)["tests"]]' | while IFS=$'\t' read -r kind a b; do
      k=$((k+1)); log=$O/M$N.test$k.log
      case $kind in
        UNIT) ex=$(run_unit "$a" "$b" "$log"); echo "    test$k UNIT $a '$b' exit $ex"; grep -E "Failure|FAILED  \]|PASSED|Expected|Actual|Which is|Value of|OK \]|error:" "$log" | grep -v "^\[ *RUN" | sed 's/^/    | /';;
        FUNC) # shellcheck disable=SC2086
              meson test -C build --no-rebuild $a > "$log" 2>&1; ex=$?; echo "    test$k FUNC '$a' exit $ex"; grep -E "^(Ok|Fail|Skipped):|OK  |FAIL " "$log" | tr -s ' ' | sed 's/^/    | /';;
        SCRIPT) "$W/$a" > "$log" 2>&1; ex=$?; echo "    test$k SCRIPT '$a' exit $ex"; sed 's/^/    | /' "$log";;
      esac
    done
  fi
  if [ -s "$O/M$N.patch" ]; then
    git apply -R "$O/M$N.patch" || { echo "    REVERT FAILED"; exit 1; }
    # shellcheck disable=SC2086
    touch $files
    # shellcheck disable=SC2086
    [ -n "$targets" ] && { ninja -C build $targets > "$O/M$N.rebuild.log" 2>&1; echo "    reverted, touched, rebuilt: exit $?"; }
  fi
  st=$(git status --short); if [ -n "$st" ]; then echo "    TREE NOT CLEAN: $st"; exit 1; else echo "    tree clean"; fi
done
