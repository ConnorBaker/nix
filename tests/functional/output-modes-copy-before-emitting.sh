#!/usr/bin/env bash

# Every output mode copies a mounted input before it emits the input's name
# (doc/lazy-store/01-specification.md, section 3, (C2), and its tightening in
# section 10; doc/lazy-store/04-derivation.md, section 1.5, "The order of copy
# and emission in `nix eval`", and the doors table's "raw output, `--write-to`,
# `nix-instantiate` lines" row).
#
# The domain is every output mode of the two commands that print a value:
# `nix eval` plain, `--json`, `--raw`, `--write-to`, and `nix-instantiate
# --eval` plain, `--xml`, `--json`, `--raw`.  They are enumerated in one array
# and driven by one loop, and the two gates below fail when a mode is added to
# either command without a row here.
#
# The check is the (C2) observer of tests/functional/differential/c2-observe.py
# in bash: a second process reads the stream the command writes and, at the
# moment the mounted input's name arrives, asks whether the object is in the
# store.  `nix-instantiate`'s `--raw` and `--xml` write through a buffered
# `std::cout`, so the value carries padding past the stdio buffer: the name
# leaves the process while the command still runs, as the observer's
# 300-derivation list makes it for the differential harness.  The input is a
# git repository, because a git input is mounted and copied on demand; a
# `path`-type input is copied by its fetcher before evaluation sees it.

source common.sh

TODO_NixOS

needLocalStore "the store is cleared between modes and the observer reads it directly"

requireGit

clearStore

# ---------------------------------------------------------------------------
# The enumeration gates.

# `nix eval`'s flags, from the command's own description.  Every flag is either
# classified here as not selecting an output mode, or driven as a mode below;
# a flag that is neither fails this test, so a new output mode cannot be added
# without a row in `modes`.
evalFlags=$(nix __dump-cli | jq -r '.args.commands.eval.flags | keys | join(" ")')
notAnOutputMode=(
    apply arg arg-from-file arg-from-stdin argstr commit-lock-file debugger
    eval-store expr file impure include inputs-from no-pretty no-registries
    no-update-lock-file no-write-lock-file output-lock-file override-flake
    override-input pretty read-only recreate-lock-file reference-lock-file
    repair update-input
)
# The `nix eval` flags this test drives as output modes.
outputModeFlags=(json raw write-to)
for flag in $evalFlags; do
    classified=
    for known in "${notAnOutputMode[@]}" "${outputModeFlags[@]}"; do
        if [[ $flag == "$known" ]]; then classified=1; break; fi
    done
    [[ -n $classified ]] || fail \
"\`nix eval\` accepts the flag '--$flag', which this test does not classify.
If it selects an output mode, add it to \`outputModeFlags\` and drive it in
\`modes\` below; otherwise add it to \`notAnOutputMode\`."
done

# `nix-instantiate` has no machine-readable flag list; its output modes are the
# members of `OutputKind`.  The source is not part of the functional-test
# package's fileset (tests/functional/package.nix), so this gate runs from a
# checkout and says so when it cannot.
instantiateSource=$_NIX_TEST_SOURCE_DIR/../../src/nix/nix-instantiate/nix-instantiate.cc
if [[ -e $instantiateSource ]]; then
    grepQuiet '^enum OutputKind { okPlain, okRaw, okXML, okJSON };$' "$instantiateSource" || fail \
"\`OutputKind\` in $instantiateSource is no longer
\`{ okPlain, okRaw, okXML, okJSON }\`.  Drive the new member in \`modes\` below."
else
    echo "the nix-instantiate source is not in this tree; OutputKind is not enumerated here" >&2
fi

# ---------------------------------------------------------------------------
# The mounted input.

repo=$TEST_ROOT/mounted-repo
rm -rf "$repo"
mkdir -p "$repo"
# Enough files that the copy takes visible time: the observer distinguishes
# copy-then-emit from emit-then-copy only while the copy is still running.
for ((i = 0; i < 2000; i++)); do
    printf 'file %d\n' "$i" > "$repo/f$i"
done
initGitRepo "$repo"
git -C "$repo" add -A
git -C "$repo" commit -q -m 'the mounted input'

fetch="(builtins.fetchGit { url = \"$repo\"; })"
# The name alone: `--read-only` makes the fetch a dry run, so the object stays
# out of the store and each mode below starts from a store that lacks it.
mounted=$(nix eval --impure --read-only --raw --expr "$fetch.outPath")
[[ -n $mounted ]]
[[ ! -e $mounted ]]

# The value every mode prints: the mounted input's name, a newline, and enough
# bytes after it to push the name past a buffered `std::cout`.
padding='(builtins.concatStringsSep "" (builtins.genList (_: "0123456789abcdef") 16384))'
expr="$fetch.outPath + \"\n\" + $padding"

# How much of the stream the observer reads before it decides.  The name starts
# within this many bytes in every mode (the longest prefix before it is
# `nix-instantiate --xml`'s document header).
prefixBytes=400

written=$TEST_ROOT/written-by-write-to

# ---------------------------------------------------------------------------
# The observer: a second process on the other end of the command's stdout.

observe() {
    local verdictFile=$1
    local chunk=
    # Returns as soon as the bytes are there, without waiting for a newline or
    # for the command to end.
    IFS= read -r -d '' -n "$prefixBytes" chunk || true
    if [[ $chunk == *"$mounted"* ]]; then
        if [[ -e $mounted ]]; then
            echo present > "$verdictFile"
        else
            echo absent > "$verdictFile"
        fi
    else
        echo "no-name-in-the-first-$prefixBytes-bytes" > "$verdictFile"
    fi
    # Drain, so that the command is never killed by SIGPIPE.
    cat > /dev/null
}

# The same reading for `--write-to`, whose escape is a file and not a stream:
# the file's appearance is the emission.
observeFile() {
    local verdictFile=$1 pid=$2 i
    echo "the command ended before the file appeared" > "$verdictFile"
    for ((i = 0; i < 2000000; i++)); do
        if [[ -e $written ]]; then
            if [[ -e $mounted ]]; then
                echo present > "$verdictFile"
            else
                echo absent > "$verdictFile"
            fi
            return
        fi
        kill -0 "$pid" 2> /dev/null || return
    done
    echo "the file did not appear" > "$verdictFile"
}

# ---------------------------------------------------------------------------
# The modes, one loop.

modes=(
    eval-plain
    eval-json
    eval-raw
    eval-write-to
    instantiate-plain
    instantiate-xml
    instantiate-json
    instantiate-raw
)

for mode in "${modes[@]}"; do
    # A store that lacks the mounted input, so that every mode pays the copy.
    clearStore
    rm -f "$written"
    verdictFile=$TEST_ROOT/verdict-$mode

    case $mode in
        eval-plain)
            cmd=(nix eval --impure --expr "$expr") ;;
        eval-json)
            cmd=(nix eval --impure --json --expr "$expr") ;;
        eval-raw)
            cmd=(nix eval --impure --raw --expr "$expr") ;;
        eval-write-to)
            cmd=(nix eval --impure --write-to "$written" --expr "$expr") ;;
        # `nix-instantiate --eval` is read-only unless asked otherwise, and a
        # read-only evaluator copies nothing (EvalState::ensureLazyPathCopied,
        # src/libexpr/paths.cc); the claim is about the writing mode.
        instantiate-plain)
            cmd=(nix-instantiate --eval --read-write-mode --expr "$expr") ;;
        instantiate-xml)
            cmd=(nix-instantiate --eval --read-write-mode --xml --expr "$expr") ;;
        instantiate-json)
            cmd=(nix-instantiate --eval --read-write-mode --json --expr "$expr") ;;
        instantiate-raw)
            cmd=(nix-instantiate --eval --read-write-mode --raw --expr "$expr") ;;
        *)
            fail "mode '$mode' has no command" ;;
    esac

    if [[ $mode == eval-write-to ]]; then
        "${cmd[@]}" > "$TEST_ROOT/out-$mode" 2> "$TEST_ROOT/err-$mode" &
        cmdPid=$!
        observeFile "$verdictFile" "$cmdPid"
        wait "$cmdPid"
        grepQuiet "^$mounted\$" "$written"
    else
        "${cmd[@]}" 2> "$TEST_ROOT/err-$mode" | observe "$verdictFile"
    fi

    verdict=$(cat "$verdictFile")
    [[ $verdict == present ]] || fail \
"in mode '$mode' the mounted input's name was emitted while the store read '$verdict': \
the copy did not precede the emission ((C2), 01-specification.md section 3)"

    # "In the store" in full: registered, not merely on the filesystem.
    nix path-info "$mounted" > /dev/null
done

echo "every output mode copied before emitting: ${modes[*]}" >&2
