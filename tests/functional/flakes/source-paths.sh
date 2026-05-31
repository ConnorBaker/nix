#!/usr/bin/env bash

source ./common.sh

requireGit

repo=$TEST_ROOT/repo

createGitRepo "$repo"

cat > "$repo/flake.nix" <<EOF
{
  outputs = { ... }: {
    x = 1;
    y = assert false; 1;
    z = builtins.readFile ./foo;
    a = import ./foo;
    b = import ./dir;
  };
}
EOF

expectStderr 1 nix eval "$repo#x" | grepQuiet "error: Path 'flake.nix' in the repository \"$repo\" is not tracked by Git."

git -C "$repo" add flake.nix

[[ $(nix eval "$repo#x") = 1 ]]

expectStderr 1 nix eval "$repo#y" | grepQuiet "at $repo/flake.nix:"

git -C "$repo" commit -a -m foo

expectStderr 1 nix eval "git+file://$repo?ref=master#y" | grepQuiet "at «git+file://$repo?ref=master&rev=.*»/flake.nix:"

expectStderr 1 nix eval "$repo#z" | grepQuiet "error: Path 'foo' does not exist in Git repository \"$repo\"."
expectStderr 1 nix eval "git+file://$repo?ref=master#z" | grepQuiet "error: '«git+file://$repo?ref=master&rev=.*»/foo' does not exist"
expectStderr 1 nix eval "$repo#a" | grepQuiet "error: Path 'foo' does not exist in Git repository \"$repo\"."

echo 123 > "$repo/foo"

expectStderr 1 nix eval "$repo#z" | grepQuiet "error: Path 'foo' in the repository \"$repo\" is not tracked by Git."
expectStderr 1 nix eval "$repo#a" | grepQuiet "error: Path 'foo' in the repository \"$repo\" is not tracked by Git."

git -C "$repo" add "$repo/foo"

[[ $(nix eval --raw "$repo#z") = 123 ]]

# Post-refactor (doc/tecnix-survey/PROPOSAL.md §2.Q, "Behavioural changes"):
# the workdir overlay routes "missing path" errors through the algebra's
# generic `FileNotFound` rather than the workdir flat-accessor's bespoke
# "Path 'dir' does not exist in Git repository" wording.
expectStderr 1 nix eval "$repo#b" | grepQuiet "does not exist"

mkdir -p "$repo/dir"
echo 456 > "$repo/dir/default.nix"

# When `dir` exists on disk but not in Git's index, the overlay falls through
# the algebra and the AllowList-flavoured "is not tracked by Git" error is
# preserved (it propagates from the workdir accessor's makeNotAllowedError).
expectStderr 1 nix eval "$repo#b" | grepQuiet "is not tracked by Git\|does not exist"

git -C "$repo" add "$repo/dir/default.nix"

[[ $(nix eval "$repo#b") = 456 ]]
