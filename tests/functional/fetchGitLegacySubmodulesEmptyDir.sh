#!/usr/bin/env bash

# Regression test: getLegacyGitAccessor silently ignores git fetch/checkout
# failures when nix-219-compat=true and submodules=true, caching an empty
# directory in the Nix store.
#
# The bug: getLegacyGitAccessor calls runProgram(RunOptions&&) for git init,
# remote add, fetch, and checkout. That overload returns the exit code in a
# pair<int,string> rather than throwing. All four return values are discarded.
# If any command fails, the temp directory contains only .git/ which is
# filtered out, producing an empty store path that gets cached permanently.

source common.sh

requireGit

clearStoreIfPossible

rootRepo=$TEST_ROOT/gitLegacySubRoot
subRepo=$TEST_ROOT/gitLegacySubSub

rm -rf "$rootRepo" "$subRepo" "$TEST_HOME"/.cache/nix

export XDG_CONFIG_HOME=$TEST_HOME/.config
git config --global protocol.file.allow always

# Create a submodule repo with content.
createGitRepo "$subRepo"
echo "submodule content" > "$subRepo"/sub.txt
git -C "$subRepo" add sub.txt
git -C "$subRepo" commit -m "Submodule initial"

# Create a root repo that references the submodule.
createGitRepo "$rootRepo"
echo "root content" > "$rootRepo"/root.txt
git -C "$rootRepo" add root.txt
git -C "$rootRepo" submodule add "$subRepo" sub
git -C "$rootRepo" commit -m "Root with submodule"

rev=$(git -C "$rootRepo" rev-parse HEAD)

# Force the remote code path (clone into cache, not direct workdir access).
export _NIX_FORCE_HTTP=1

# Create a git wrapper that fails the legacy-path fetch and logs
# every invocation so we can verify it was actually used.
#
# getLegacyGitAccessor with submodules runs:
#   git init <tmpdir>
#   git -C <tmpdir> remote add origin <cache-dir>
#   git -C <tmpdir> fetch --quiet origin <rev>
#   git -C <tmpdir> checkout --quiet <rev>
#
# The main cache fetch (GitRepoImpl::fetch) runs:
#   git -C <cache-dir> --git-dir . fetch --progress --force ...
#
# We distinguish them: legacy uses "fetch --quiet" without "--git-dir".
wrapperDir="$TEST_ROOT/bin"
wrapperLog="$TEST_ROOT/git-wrapper.log"
mkdir -p "$wrapperDir"
: > "$wrapperLog"

real_git=$(type -P git)
real_bash=$(type -P bash)

cat > "$wrapperDir/git" <<EOF
#!$real_bash
echo "git-wrapper: \$*" >> "$wrapperLog"

has_fetch=0
has_quiet=0
has_git_dir=0
for arg in "\$@"; do
    case "\$arg" in
        fetch) has_fetch=1 ;;
        --quiet) has_quiet=1 ;;
        --git-dir) has_git_dir=1 ;;
    esac
done

# Legacy fetch signature: "fetch" + "--quiet" but no "--git-dir"
if [[ \$has_fetch -eq 1 && \$has_quiet -eq 1 && \$has_git_dir -eq 0 ]]; then
    echo "git-wrapper: INTERCEPTED legacy fetch" >> "$wrapperLog"
    exit 128
fi

exec "$real_git" "\$@"
EOF
chmod +x "$wrapperDir/git"

saved_PATH="$PATH"
export PATH="$wrapperDir:$PATH"

# Run the fetch from scratch with the wrapper active from the start.
# No prior fetch has happened, so there are no cached store paths to
# bypass the legacy code path. The wrapper is in PATH before nix eval
# is invoked, so all git subprocesses will use it.
path_bad=$(nix eval --nix-219-compat --raw --expr \
    "(builtins.fetchGit { url = file://$rootRepo; rev = \"$rev\"; submodules = true; }).outPath" \
    2>"$TEST_ROOT/stderr.log") && fetch_ok=1 || fetch_ok=0

export PATH="$saved_PATH"
rm -f "$wrapperDir/git"

# Diagnostic: show what the wrapper saw.
echo "=== git wrapper log ===" >&2
cat "$wrapperLog" >&2 || true
echo "=== end wrapper log ===" >&2

# Verify the wrapper actually intercepted something.
if ! grep -q "INTERCEPTED" "$wrapperLog" 2>/dev/null; then
    echo "=== nix eval stderr ===" >&2
    cat "$TEST_ROOT/stderr.log" >&2 || true
    echo "=== end stderr ===" >&2
    fail "git wrapper never intercepted a legacy fetch — test setup is broken"
fi

if [[ $fetch_ok -eq 1 ]]; then
    # Eval "succeeded" — it must not have produced an empty directory.
    if [[ ! -f "$path_bad/root.txt" ]]; then
        echo "BUG: fetch appeared to succeed but store path is an empty directory" >&2
        echo "  store path: $path_bad" >&2
        ls -la "$path_bad" >&2 || true
        fail "getLegacyGitAccessor silently cached an empty directory"
    fi
    [[ $(cat "$path_bad/root.txt") = "root content" ]] \
        || fail "root.txt has wrong content"
else
    # Eval failed. With the bug present, we expect eval to SUCCEED (with
    # an empty dir), not fail. So either the bug is fixed (git failure
    # is now propagated) or the wrapper broke something else.
    echo "=== nix eval stderr ===" >&2
    cat "$TEST_ROOT/stderr.log" >&2 || true
    echo "=== end stderr ===" >&2
    fail "nix eval failed unexpectedly (with the bug present, it should succeed with an empty dir)"
fi
