#!/usr/bin/env bash
# The fetcher cache's domains and the consumers of their rows
# (doc/lazy-store/01-specification.md sections 9.9 and 9.11;
# doc/lazy-store/05-validation.md section 8, rule 1).  A cache domain is a
# bare string literal at a `Cache::Key` construction and a row's attributes
# are bare string literals at `getStrAttr`/`getIntAttr`/`getBoolAttr`; there
# is no enum, no constexpr array and no switch over them anywhere
# (`src/libfetchers/include/nix/fetchers/cache.hh`: `using Domain =
# std::string_view`), so neither the compiler nor a unit test can enumerate
# them.  The lists below are the enumeration; this script checks them
# against the tree and exits 1 naming whatever differs, in either direction
# -- a consumer added without a line here, and a line here whose consumer is
# gone.  Run from any directory.
#
# The scan strips `//` comments and reads each file as a rolling buffer, so
# a key whose literal stands on the line after `Cache::Key {` is seen (a
# one-line shape of the construction would miss it).  A domain whose key is
# built by a named function is consumed wherever that function is called and
# not only where its literal appears, so those functions are scanned for as
# well: the callers of the primitive that produces the value, not of a
# wrapper that may be inlined.
set -euo pipefail
cd "$(dirname "$0")/.."
status=0

# The production files: the tests have domains of their own ("test",
# "other") and read rows in order to check them, which is not a consumer to
# track.
production_files() {
    find src -type f \( -name '*.cc' -o -name '*.hh' -o -name '*.h' \) \
        | grep -vE '^src/[^/]*-(tests|test-support)/' \
        | sort
}

# One pass over every production file: `DOM <file> <domain>` for each cache
# domain constructed, `FUN <file> <function>` for each call of a function
# that builds or reads one of the tracked rows, `ATTR <file> <attribute>`
# for each read of a tracked row attribute out of an `Attrs`.
scan() {
    # shellcheck disable=SC2016 # the awk program's `$0` is awk's, not the shell's
    production_files | xargs awk '
        FNR == 1 { buf = "" }
        {
            code = $0
            sub(/\/\/.*/, "", code)
            buf = buf " " code
            if (length(buf) > 800)
                buf = substr(buf, length(buf) - 400)
            while (match(buf, /Cache::Key[A-Za-z_ ]*\{[ \t]*"[A-Za-z]+"/)) {
                m = substr(buf, RSTART, RLENGTH)
                buf = substr(buf, RSTART + RLENGTH)
                sub(/"$/, "", m)
                sub(/^.*"/, "", m)
                print "DOM\t" FILENAME "\t" m
            }
            if (match(code, /(^|[^A-Za-z0-9_])(makeSourcePathToHashCacheKey|recordRootEntry|lookupTreeAddress|recordTreeAddress)[ \t]*\(/)) {
                f = substr(code, RSTART, RLENGTH)
                gsub(/[^A-Za-z]/, "", f)
                print "FUN\t" FILENAME "\t" f
            }
            if (match(code, /(maybe)?[gG]et(Str|Int|Bool)Attr\([^,]*,[ \t]*"(hash|mode|mentions|treeHash)"/)) {
                a = substr(code, RSTART, RLENGTH)
                sub(/"$/, "", a)
                sub(/^.*"/, "", a)
                print "ATTR\t" FILENAME "\t" a
            }
        }
    '
}

scanned=$(scan)

# The files a kind/name pair was seen in, one per line.
seen() {
    printf '%s\n' "$scanned" | awk -F'\t' -v kind="$1" -v name="$2" '$1 == kind && $3 == name { print $2 }' | sort -u
}

# `<domain><TAB><file>` for every file naming `$1` as a domain or calling
# one of the functions in `$2` (space-separated, possibly empty).
consumers_of() {
    local domain=$1 fn
    shift
    {
        seen DOM "$domain"
        for fn in "$@"; do
            seen FUN "$fn"
        done
    } | sort -u | sed "s,^,$domain\t,"
}

# `diff` of what is declared against what the tree has, under a heading.
compare() {
    local what=$1 declared=$2 found=$3 d
    d=$(diff <(printf '%s\n' "$declared" | sort -u) <(printf '%s\n' "$found" | sort -u) || true)
    if [[ -n $d ]]; then
        echo "ERROR: $what"
        echo "       '<' is declared here and not in the tree; '>' is in the tree and not declared."
        echo "$d"
        status=1
    fi
}

# ---------------------------------------------------------------------------
# 1. Every cache domain the tree constructs is declared.
# ---------------------------------------------------------------------------
compare "the fetcher cache's domains have changed (cache.hh, Cache::Key):" \
    "$(
        cat <<'EOF'
file
gitLastModified
gitRevCount
gitRevToLastModified
gitRevToTreeHash
hgRefToRev
hgRev
sourcePathToHash
tarball
treeAddress
treeMentionsAttribute
EOF
    )" \
    "$(printf '%s\n' "$scanned" | awk -F'\t' '$1 == "DOM" { print $3 }')"

# ---------------------------------------------------------------------------
# 2. Every consumer of the rows the specification names is declared:
#    `sourcePathToHash` and `treeAddress` (the memos of 01 section 9.9),
#    `treeMentionsAttribute` (the export-ignore answer), and the two rows
#    that carry a `treeHash` attribute.
# ---------------------------------------------------------------------------
compare "the consumers of a fetcher-cache row have changed:" \
    "$(
        cat <<'EOF'
sourcePathToHash	src/libfetchers/fetch-to-store.cc
sourcePathToHash	src/libfetchers/fetchers.cc
sourcePathToHash	src/libfetchers/include/nix/fetchers/fetch-to-store.hh
sourcePathToHash	src/libfetchers/path.cc
treeAddress	src/libexpr/primops.cc
treeAddress	src/libexpr/primops/fetchTree.cc
treeAddress	src/libfetchers/fetch-to-store.cc
treeAddress	src/libfetchers/fetchers.cc
treeAddress	src/libfetchers/include/nix/fetchers/fetch-to-store.hh
treeMentionsAttribute	src/libfetchers/git.cc
gitRevToTreeHash	src/libfetchers/github.cc
tarball	src/libfetchers/tarball.cc
EOF
    )" \
    "$(
        consumers_of sourcePathToHash makeSourcePathToHashCacheKey recordRootEntry
        consumers_of treeAddress lookupTreeAddress recordTreeAddress
        consumers_of treeMentionsAttribute
        consumers_of gitRevToTreeHash
        consumers_of tarball
    )"

# ---------------------------------------------------------------------------
# 3. Every reader of an attribute those rows carry is declared.  `treeHash`
#    is also the name of an `Input` attribute, read with the same accessor;
#    that reader is listed too, since the scan cannot tell the two apart and
#    what is checked is the list.
# ---------------------------------------------------------------------------
compare "the readers of a fetcher-cache row's attributes have changed:" \
    "$(
        cat <<'EOF'
hash	src/libfetchers/fetch-to-store.cc
mentions	src/libfetchers/git.cc
mode	src/libfetchers/fetch-to-store.cc
treeHash	src/libfetchers/fetchers.cc
treeHash	src/libfetchers/github.cc
treeHash	src/libfetchers/tarball.cc
EOF
    )" \
    "$(printf '%s\n' "$scanned" | awk -F'\t' '$1 == "ATTR" { print $3 "\t" $2 }')"

# ---------------------------------------------------------------------------
# The self-test: the three checks must report five planted consumers in a
# scratch copy of the tree, or they have not been shown to run (a check that
# never fails on a plant is indistinguishable from one that is inert).  Runs
# after the checks unless CHECK_CACHE_DOMAINS_NO_SELF_TEST is set, which the
# recursive invocation sets.
# ---------------------------------------------------------------------------
self_test() {
    local tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN
    mkdir -p "$tmp/src" "$tmp/maintainers"
    cp -R src/. "$tmp/src/"
    cp maintainers/check-cache-domains.sh "$tmp/maintainers/"
    # 1. a domain nobody declared
    printf '\nstatic void planted1() { Cache::Key k{"plantedDomain", {{"x", "y"}}}; }\n' \
        >> "$tmp/src/libfetchers/path.cc"
    # 2. a consumer of `sourcePathToHash` in a file not on the list
    printf '\nstatic void planted2() { makeSourcePathToHashCacheKey(f, m, p); }\n' \
        >> "$tmp/src/libfetchers/git.cc"
    # 3. a consumer of `treeAddress` in a file not on the list
    printf '\nstatic void planted3() { lookupTreeAddress(settings, h); }\n' \
        >> "$tmp/src/libfetchers/tarball.cc"
    # 4. a consumer of `treeMentionsAttribute` in a file not on the list, its
    #    literal on the line after the brace -- the shape a line-by-line
    #    check would miss
    printf '\nstatic void planted4() { Cache::Key k{\n    "treeMentionsAttribute", {{"rev", r}}}; }\n' \
        >> "$tmp/src/libfetchers/github.cc"
    # 5. a reader of the `treeHash` attribute in a file not on the list
    printf '\nstatic void planted5() { getStrAttr(attrs, "treeHash"); }\n' \
        >> "$tmp/src/libfetchers/mercurial.cc"

    local out rc=0
    out=$(cd "$tmp" && CHECK_CACHE_DOMAINS_NO_SELF_TEST=1 bash maintainers/check-cache-domains.sh 2>&1) || rc=$?
    local expect missing=""
    while IFS= read -r expect; do
        grep -qF "$expect" <<< "$out" || missing+=" [$expect]"
    done <<EOF
> plantedDomain
> sourcePathToHash	src/libfetchers/git.cc
> treeAddress	src/libfetchers/tarball.cc
> treeMentionsAttribute	src/libfetchers/github.cc
> treeHash	src/libfetchers/mercurial.cc
EOF
    if [[ $rc -ne 1 || -n $missing ]]; then
        echo "ERROR: the self-test did not report every planted consumer (exit $rc; missing:$missing):"
        echo "$out"
        return 1
    fi
}

if [[ $status -eq 0 && -z ${CHECK_CACHE_DOMAINS_NO_SELF_TEST:-} ]]; then
    self_test || status=1
fi

exit $status
