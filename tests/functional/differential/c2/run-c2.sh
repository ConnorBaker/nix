#!/usr/bin/env bash
# run-c2.sh <label> <nix package> <mode>
#
# Runs one command against a fresh private store under $C2_ROOT (required) and pipes the
# stream that carries store paths (stdout, or stderr for trace and throw)
# through the (C2) observer as the command runs; prints the observer's tally.
# The list of 300 derivations in many-drvs.nix is long enough that a
# serialisation of it exceeds a pipe's stdio buffer, so bytes leave the process
# before the command ends.  Modes: xml raw json (nix-instantiate --eval, which
# is read-only, so nothing is in Delta and master is absent too), drvs
# (nix-instantiate), eval evaljson evalraw (nix eval), match split getctx
# trace throw (nix eval through a context-dropping builtin, trace, or an error).
# The differential reading: the implementation must never be absent where the
# reference is present.
set -euo pipefail
label=$1; pkg=$2; mode=$3
H=$(cd "$(dirname "$0")/.." && pwd)
root=${C2_ROOT:?set C2_ROOT to a scratch directory}/$label-$mode
[ -d "$root" ] && chmod -R u+w "$root"; rm -rf "$root"; mkdir -p "$root/store" "$root/var/nix" "$root/etc"
export NIX_STORE_DIR=$root/store NIX_STATE_DIR=$root/var/nix NIX_LOG_DIR=$root/var/log/nix NIX_CONF_DIR=$root/etc
export NIX_DATA_DIR=$root/share NIX_LOCALSTATE_DIR=$root/var
NIX_CONFIG="experimental-features = nix-command flakes
sandbox = false
substituters ="
export NIX_CONFIG
export PATH=$pkg/bin:$PATH
stream=stdout
bigdir() {
  # A git repository, because git inputs are mounted and copied on demand; a
  # plain path input is copied by its fetcher before evaluation sees it.
  mkdir -p "$root/bigdir"
  for i in $(seq 1 4000); do head -c 1024 /dev/zero | tr '\0' "$(printf '\x41')" > "$root/bigdir/f$i"; echo "$i" >> "$root/bigdir/f$i"; done
  (cd "$root/bigdir" && git init -q && git add -A && git -c user.name=c2 -c user.email=c2@example.org commit -qm init) > /dev/null
}
F=$H/c2/many-drvs.nix
case $mode in
  xml)   cmd=(nix-instantiate --eval --strict --xml "$F") ;;
  raw)   cmd=(nix-instantiate --eval --raw -E "builtins.concatStringsSep \"\n\" (map (d: d.drvPath) (import (/. + \"$F\")))") ;;
  json)  cmd=(nix-instantiate --eval --strict --json -E "map (d: d.drvPath) (import (/. + \"$F\"))") ;;
  drvs)  cmd=(nix-instantiate "$F") ;;
  eval)  cmd=(nix eval --file "$F" --apply "map (d: d.drvPath)") ;;
  # Reachable eliminators of a writing command: the path leaves through the
  # context-dropping builtins, through trace on stderr, or through an error.
  match) cmd=(nix eval --file "$F" --apply "map (d: builtins.head (builtins.match \"(.*)\" d.drvPath))") ;;
  split) cmd=(nix eval --file "$F" --apply "map (d: builtins.elemAt (builtins.split \"(.*)\" d.drvPath) 1)") ;;
  getctx) cmd=(nix eval --file "$F" --apply "map (d: builtins.attrNames (builtins.getContext d.drvPath))") ;;
  trace) stream=stderr; cmd=(nix eval --file "$F" --apply "ds: builtins.foldl' (a: d: builtins.trace d.drvPath a) 1 ds") ;;
  throw) stream=stderr; cmd=(nix eval --file "$F" --apply "ds: throw (builtins.concatStringsSep \"\n\" (map (d: d.drvPath) ds))") ;;
  evaljson) cmd=(nix eval --json --file "$F" --apply "map (d: d.drvPath)") ;;
  # A mounted (lazily copied) git input printed by nix eval: master prints
  # the path and copies afterwards; the branch copies first.  The repository
  # is large enough that the copy takes visible time.
  lazyraw)  bigdir; cmd=(nix eval --impure --raw --expr "(builtins.fetchGit { url = \"$root/bigdir\"; }).outPath + \"\n\"") ;;
  lazyeval) bigdir; cmd=(nix eval --impure --expr "(builtins.fetchGit { url = \"$root/bigdir\"; }).outPath") ;;
  lazyjson) bigdir; cmd=(nix eval --impure --json --expr "(builtins.fetchGit { url = \"$root/bigdir\"; }).outPath") ;;
  evalraw) cmd=(nix eval --raw --file "$F" --apply "ds: builtins.concatStringsSep \"\n\" (map (d: d.drvPath) ds)") ;;
  # The repl's error print, while the evaluator lives on: the error names every
  # derivation path and is read on stderr as the repl prints it.
  repl)  stream=stderr; stdin_input="throw (builtins.concatStringsSep \"\\n\" (map (d: d.drvPath) ds))"; cmd=(nix repl --expr "{ ds = import (/. + \"$F\"); }") ;;
esac
stdin_input=${stdin_input:-}
if [ "$stream" = stdout ]; then
  "${cmd[@]}" 2> "$root/stderr" | python3 "$H/c2-observe.py" "$root/report" > "$root/stdout" || true
elif [ -n "$stdin_input" ]; then
  printf '%s\n' "$stdin_input" | "${cmd[@]}" 2>&1 1> "$root/stdout" | python3 "$H/c2-observe.py" "$root/report" > "$root/stderr" || true
else
  "${cmd[@]}" 2>&1 1> "$root/stdout" | python3 "$H/c2-observe.py" "$root/report" > "$root/stderr" || true
fi
echo "$label $mode: present=$(grep -c ' present$' "$root/report" || true) absent=$(grep -c ' absent$' "$root/report" || true) stdout_bytes=$(wc -c < "$root/stdout") stderr_lines=$(wc -l < "$root/stderr")"
