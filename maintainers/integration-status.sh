#!/usr/bin/env bash
# Report how the branches merged into an integration build compare with
# their current tips. See INTEGRATION.md. No network access: fetch first to
# compare against the remotes' current state.
#
# Usage: maintainers/integration-status.sh [<integration commit>]   (default: HEAD)
#
# Exits 0 when every merged branch and the base are up to date and the
# merges agree with INTEGRATION.md's table, and 1 otherwise.

set -euo pipefail

rev=${1:-HEAD}
head=$(git rev-parse --verify --quiet "$rev^{commit}") || {
    echo "integration-status: '$rev' is not a commit" >&2
    exit 2
}
manifest=$(git show "$head:INTEGRATION.md" 2>/dev/null) || {
    echo "integration-status: $rev has no INTEGRATION.md" >&2
    exit 2
}

problems=0

# shellcheck disable=SC2016 # the backticks are literal
base=$(sed -n 's/^Base `master` commit: `\([0-9a-f]\{40\}\)`$/\1/p' <<<"$manifest")
if [[ -z $base ]]; then
    echo "integration-status: INTEGRATION.md names no base commit" >&2
    exit 2
fi

# The merges, oldest first, from the first-parent history above the base:
# "<branch> <second parent>".
declare -a merged_branches=() merged_shas=()
on_chain=false
while read -r commit _ parent2 rest; do
    if [[ $commit == "$base" ]]; then
        on_chain=true
        break
    fi
    subject=$(git log -1 --format=%s "$commit")
    if [[ -n $parent2 ]]; then
        if [[ -n $rest ]]; then
            echo "warning: $commit is an octopus merge; only its second parent is checked"
        fi
        if [[ $subject =~ ^Merge\ ([^ ]+)\ \([0-9a-f]+\)$ ]]; then
            merged_branches=("${BASH_REMATCH[1]}" "${merged_branches[@]}")
            merged_shas=("$parent2" "${merged_shas[@]}")
        else
            echo "MISMATCH: merge $commit has an unexpected subject: $subject"
            problems=1
        fi
    elif [[ $commit != "$head" || $subject != "integration: describe the merged branches" ]]; then
        echo "note: integration-only commit $(git rev-parse --short "$commit") $subject"
    fi
done < <(git rev-list --first-parent --parents "$head")
if ! $on_chain; then
    echo "MISMATCH: base $base is not on the first-parent history of $rev"
    problems=1
fi

# Cross-check against the table: "| <n> | `branch` | `sha` | ...".
declare -a table_branches=() table_shas=()
while IFS='|' read -r _ order branch sha _; do
    order=${order//[[:space:]]/}
    [[ $order =~ ^[0-9]+$ ]] || continue
    branch=${branch//[[:space:]\`]/}
    sha=${sha//[[:space:]\`]/}
    table_branches[order - 1]=$branch
    table_shas[order - 1]=$sha
done <<<"$manifest"

if ((${#table_branches[@]} != ${#merged_branches[@]})); then
    echo "MISMATCH: the table lists ${#table_branches[@]} branches, the history has ${#merged_branches[@]} merges"
    problems=1
fi
for i in "${!merged_branches[@]}"; do
    if [[ ${table_branches[i]-} != "${merged_branches[i]}" || ${table_shas[i]-} != "${merged_shas[i]}" ]]; then
        echo "MISMATCH: merge $((i + 1)) is ${merged_branches[i]} at ${merged_shas[i]}," \
            "the table says ${table_branches[i]-(nothing)} at ${table_shas[i]-(nothing)}"
        problems=1
    fi
done

# status <merged sha> <tip>
status() {
    if [[ $2 == "$1" ]]; then
        echo "UP-TO-DATE"
    elif git merge-base --is-ancestor "$1" "$2"; then
        echo "BEHIND ($(git rev-list --count "$1..$2") commits)"
    else
        echo "REWRITTEN"
    fi
}

echo
echo "Integration $(git rev-parse --short "$head"), base $(git rev-parse --short "$base")"
echo
for i in "${!merged_branches[@]}"; do
    branch=${merged_branches[i]}
    sha=${merged_shas[i]}
    echo "$((i + 1)). $branch merged at $sha"
    found=false
    for ref in "refs/heads/$branch" "refs/remotes/origin/$branch"; do
        tip=$(git rev-parse --verify --quiet "$ref^{commit}") || continue
        found=true
        st=$(status "$sha" "$tip")
        [[ $st == UP-TO-DATE ]] || problems=1
        printf '    %-45s %s  %s\n' "${ref#refs/*/}" "$tip" "$st"
    done
    if ! $found; then
        echo "    MISSING (neither $branch nor origin/$branch exists)"
        problems=1
    fi
done

echo
echo "Base $base"
for ref in refs/heads/master refs/remotes/origin/master refs/remotes/upstream/master; do
    tip=$(git rev-parse --verify --quiet "$ref^{commit}") || continue
    if git merge-base --is-ancestor "$base" "$tip"; then
        st=$(status "$base" "$tip")
        [[ $st == UP-TO-DATE ]] || problems=1
        printf '    %-45s %s  %s\n' "${ref#refs/*/}" "$tip" "$st"
    else
        problems=1
        printf '    %-45s %s  %s\n' "${ref#refs/*/}" "$tip" "REWRITTEN (does not contain the base)"
    fi
done

exit "$problems"
