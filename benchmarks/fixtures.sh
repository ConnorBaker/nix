#!/usr/bin/env bash
# Generate the git-repo fixtures the workloads evaluate against.
# Deterministic content (no timestamps in file bodies) so store paths are stable
# across runs and trees. Fixtures live under benchmarks/.fixtures/ (gitignored).
#
# Usage: fixtures.sh [dir]    (default: benchmarks/.fixtures)

set -euo pipefail
BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
FX="${1:-$BENCH_DIR/.fixtures}"

# Fixture scale. Defaults are tiny (fast semantic runs — walk/copy COUNTS don't
# need bulk). For meaningful WALL-CLOCK, set BENCH_SCALE=big (or tune the knobs):
#   FILE_BYTES  bytes per file   FILES_PER_DIR  files per package/subdir
# At big scale the copy-once-link-N win (1 copy vs N) becomes visible in seconds.
case "${BENCH_SCALE:-small}" in
    big)  : "${FILE_BYTES:=50000}"  "${FILES_PER_DIR:=40}" ;;
    *)    : "${FILE_BYTES:=64}"     "${FILES_PER_DIR:=8}"  ;;
esac

# Deterministic git identity + dates so commit hashes are reproducible.
git_init() {  # <repo>
    rm -rf "$1"; mkdir -p "$1"
    git init -q "$1"
    git -C "$1" config user.email bench@example.com
    git -C "$1" config user.name  Bench
    git -C "$1" config commit.gpgsign false
}
git_commit() {  # <repo> <msg>
    GIT_AUTHOR_DATE='2026-01-01T00:00:00 +0000' \
    GIT_COMMITTER_DATE='2026-01-01T00:00:00 +0000' \
    git -C "$1" -c user.name=Bench -c user.email=bench@example.com commit -qm "$2"
}

# A directory of N text files. Each file is padded to ~FILE_BYTES with
# DETERMINISTIC content (no randomness, no timestamps) so store paths/tree-SHAs
# are stable across runs and trees. The leading line keeps files distinct.
# Filler is built without pipes (a `yes|head` pipe trips SIGPIPE under pipefail).
gen_files() {  # <dir> <n> <prefix>
    mkdir -p "$1"
    local i
    # Build one deterministic filler block of ~FILE_BYTES once, reuse per file.
    local block="" unit
    if (( FILE_BYTES > 64 )); then
        unit="deterministic-filler-0123456789-"
        while (( ${#block} < FILE_BYTES )); do block+="$unit"; done
        block="${block:0:FILE_BYTES}"
    fi
    for ((i = 1; i <= $2; i++)); do
        {
            printf 'fixture %s file %d\n' "$3" "$i"
            if [[ -n "$block" ]]; then printf '%s\n' "$block"; fi
        } > "$1/f$i.txt"
    done
}

echo "Generating fixtures under: $FX"
rm -rf "$FX"; mkdir -p "$FX"

# ---------------------------------------------------------------------------
# Fixture 1: shared-subtree-across-revs  (README §6.1 / PROPOSAL cross-rev bridge)
# Two commits whose `sub/` subtree is byte-identical; only `other/` differs.
# A subtree-keyed cache should walk `sub/` once across both revs.
# ---------------------------------------------------------------------------
R1="$FX/shared-subtree"
git_init "$R1"
gen_files "$R1/sub"   "$FILES_PER_DIR" stable   # the shared subtree
gen_files "$R1/other" 5 rev-a
git -C "$R1" add -A; git_commit "$R1" "rev a"
REV_A=$(git -C "$R1" rev-parse HEAD)
gen_files "$R1/other" 5 rev-b          # change ONLY other/ ; sub/ untouched
git -C "$R1" add -A; git_commit "$R1" "rev b"
REV_B=$(git -C "$R1" rev-parse HEAD)
# sanity: subtree SHA identical across revs
S_A=$(git -C "$R1" rev-parse "$REV_A:sub"); S_B=$(git -C "$R1" rev-parse "$REV_B:sub")
[[ "$S_A" == "$S_B" ]] || { echo "FIXTURE BUG: sub/ SHA differs across revs ($S_A vs $S_B)"; exit 1; }

# ---------------------------------------------------------------------------
# Fixture 2: monorepo-N-subtrees  (PROPOSAL §4 cargo workspace)
# One repo, N distinct package subdirs under pkgs/. Each is a builtins.path with
# a distinct name → N distinct CA paths, but sharing one source content id.
# ---------------------------------------------------------------------------
R2="$FX/monorepo"
git_init "$R2"
N_PKGS="${MONO_PKGS:-12}"
for ((p = 1; p <= N_PKGS; p++)); do
    gen_files "$R2/pkgs/pkg$p" "$FILES_PER_DIR" "pkg$p"
done
git -C "$R2" add -A; git_commit "$R2" "monorepo"
REV_MONO=$(git -C "$R2" rev-parse HEAD)

# ---------------------------------------------------------------------------
# Fixture 3: general-eval flake  (broad "general path performance" case)
# A flake.nix with a modest attr tree referencing many paths under src/, plus a
# lib.cleanSource-style filtered src — exercises the common nixpkgs idiom shape.
# ---------------------------------------------------------------------------
R3="$FX/flake"
git_init "$R3"
gen_files "$R3/src" 60 flakesrc
mkdir -p "$R3/src/nested"; gen_files "$R3/src/nested" 20 nested
cat > "$R3/flake.nix" <<'NIX'
{
  description = "bench general-eval flake";
  outputs = { self }: {
    # A path-heavy attr tree: many leaves are paths into src/.
    files = builtins.listToAttrs (map
      (n: { name = "f${toString n}"; value = ./src + "/f${toString n}.txt"; })
      (builtins.genList (i: i + 1) 60));
    # The dominant nixpkgs idiom: a filtered source of the whole tree.
    cleaned = builtins.path {
      path = ./.;
      name = "flake-cleaned";
      filter = path: type: builtins.baseNameOf path != ".git";
    };
    sub = builtins.path { path = ./src/nested; name = "nested"; };
  };
}
NIX
# A JSON file for the parse-cache workload (fromJSON over readFile).
cat > "$R3/data.json" <<'JSON'
{ "alpha": 1, "beta": [2, 3, 4], "gamma": { "nested": true }, "delta": "a string value", "epsilon": null }
JSON
git -C "$R3" add -A; git_commit "$R3" "flake"
REV_FLAKE=$(git -C "$R3" rev-parse HEAD)

# ---------------------------------------------------------------------------
# Record fixture metadata for the workload scripts to source.
# ---------------------------------------------------------------------------
cat > "$FX/meta.env" <<META
# auto-generated by fixtures.sh — paths + revs for the workloads
FX_ROOT=$FX
SHARED_REPO=$R1
SHARED_REV_A=$REV_A
SHARED_REV_B=$REV_B
SHARED_SUBTREE_SHA=$S_A
MONO_REPO=$R2
MONO_REV=$REV_MONO
MONO_N_PKGS=$N_PKGS
FLAKE_REPO=$R3
FLAKE_REV=$REV_FLAKE
META

echo "  shared-subtree: revA=$REV_A revB=$REV_B  (sub/ SHA=$S_A, identical ✓)"
echo "  monorepo:       rev=$REV_MONO  ($N_PKGS packages)"
echo "  flake:          rev=$REV_FLAKE"
echo "Done. Metadata: $FX/meta.env"
