#!/usr/bin/env bash

source common.sh

# Traversals evaluate the elements of their root in unforce jobs (builds
# without Boehm GC), and print what they print without. An element runs in
# the job of the one before unless memory in use (what ended jobs keep, and
# the open job's top) rose past its most at any earlier boundary, or the
# open job holds more than any one element took: so the first element's job
# ends, and a later element that allocates no more than that joins the open
# job (one job fewer than elements below).

clearStoreIfPossible

# The number of unforce jobs that `$@` ran, or "-" in a build with Boehm GC,
# which has none: its statistics have "gc" in place of "unforce".
jobsOf() {
    NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_ROOT/stats.json" "$@" > "$TEST_ROOT/out"
    jq -r 'if has("gc") then "-" else .unforce.jobs end' "$TEST_ROOT/stats.json"
}

expectJobs() {
    [[ $1 == - || $1 == "$2" ]] || fail "expected $2 unforce jobs, got $1"
}

# A set printed in an earlier element's job is «repeated» in a later one's;
# b builds s again, no more than a did, so c runs in b's job.
expr='let s = { x = 1; }; in { a = s; b = s; c = [ 1 2 3 ]; }'
expectJobs "$(jobsOf nix eval --expr "$expr")" 2
[[ $(< "$TEST_ROOT/out") == '{ a = { x = 1; }; b = «repeated»; c = [ 1 2 3 ]; }' ]]
# Strict JSON printing nests: an element of a printed set or list is a job of
# its own too, once its parent's job is not empty (a's x, c's first element).
expectJobs "$(jobsOf nix eval --json --expr "$expr")" 4
[[ $(< "$TEST_ROOT/out") == '{"a":{"x":1},"b":{"x":1},"c":[1,2,3]}' ]]
# nix-instantiate evaluates its root, and then each attribute path, in a job
# of its own first.
expectJobs "$(jobsOf nix-instantiate --eval --strict --xml -E "$expr")" 4
expectJobs "$(jobsOf nix repl <<< ":p $expr")" 2

# --strict forces each element in a job before anything is printed: what
# it forced stays forced (a reverted cell prints as `<CODE>`), and what an
# element built besides is freed.
expectJobs "$(jobsOf nix-instantiate --eval --strict -E "$expr")" 4
[[ $(< "$TEST_ROOT/out") == '{ a = { x = 1; }; b = «repeated»; c = [ 1 2 3 ]; }' ]]
expectJobs "$(jobsOf nix-instantiate --eval --strict -E '{ n = builtins.length (builtins.genList (x: x) 100); }')" 3
[[ $(< "$TEST_ROOT/out") == '{ n = 100; }' ]]
# unforce-always-revert keeps what the traversal walked, as without it (s's
# value, a string, is in the job's memory).
[[ $(nix-instantiate --eval --strict --option unforce-always-revert true -E '{ s = "a" + "b"; }') == '{ s = "ab"; }' ]]
# Only on 64-bit Linux, without Boehm GC, do jobs have a region, from which
# they free.
if [[ $system == *-linux && $system != i686-linux ]] && ! jq -e 'has("gc")' "$TEST_ROOT/stats.json" > /dev/null; then
    [[ $(jq -r '.unforce.bytesFreed' "$TEST_ROOT/stats.json") -gt 0 ]]
fi
# Each element here is a string, a name read from a set its job built, which
# the job keeps by a copy: it frees the set and its list, and the later
# elements' jobs, which reuse that memory, leave the names intact.
nexpr='builtins.mapAttrs (_: p: builtins.seq (builtins.length p.big) p.name) (builtins.listToAttrs (builtins.genList (i: { name = "p${toString i}"; value = { name = "n-" + toString i; big = builtins.genList (x: x) 1000; }; }) 3))'
expectJobs "$(jobsOf nix-instantiate --eval --strict -E "$nexpr")" 4
[[ $(< "$TEST_ROOT/out") == '{ p0 = "n-0"; p1 = "n-1"; p2 = "n-2"; }' ]]
if [[ $system == *-linux && $system != i686-linux ]] && ! jq -e 'has("gc")' "$TEST_ROOT/stats.json" > /dev/null; then
    [[ $(jq -r '.unforce.bytesFreed > .unforce.bytesKept' "$TEST_ROOT/stats.json") == true ]]
fi

# A cell an element's job reverted (s in a's) is kept by the next job that
# forces it (b's and c's); with unforce-always-revert, every job reverts it
# again. The output is the same.
rexpr='let s = { x = 1; }; in { a = s.x; b = s.x; c = s.x; }'
expectJobs "$(jobsOf nix eval --expr "$rexpr")" 2
[[ $(< "$TEST_ROOT/out") == '{ a = 1; b = 1; c = 1; }' ]]
reverted=$(jq -r '.unforce.cellsReverted' "$TEST_ROOT/stats.json")
expectJobs "$(jobsOf nix eval --option unforce-always-revert true --expr "$rexpr")" 2
[[ $(< "$TEST_ROOT/out") == '{ a = 1; b = 1; c = 1; }' ]]
if ! jq -e 'has("gc")' "$TEST_ROOT/stats.json" > /dev/null; then
    [[ $(jq -r '.unforce.cellsReverted' "$TEST_ROOT/stats.json") -gt $reverted ]]
fi
# e2, in e1's job, forces an element of x, which e1 forced: the job keeps x,
# which e3 reads; with unforce-always-revert, it reverts x, and e3 forces x
# again.
xexpr='let x = builtins.trace "x" (builtins.genList (i: i * 2) 3); in { e0 = builtins.length (builtins.genList (i: i) 1000); e1 = builtins.length x; e2 = builtins.seq (builtins.elemAt x 1) (builtins.length (builtins.genList (i: i) 2000)); e3 = builtins.length x; }'
expectJobs "$(jobsOf nix eval --expr "$xexpr" 2> "$TEST_ROOT/err")" 3
[[ $(grep -c '^trace: x$' "$TEST_ROOT/err") == 1 ]]
expectJobs "$(jobsOf nix eval --option unforce-always-revert true --expr "$xexpr" 2> "$TEST_ROOT/err")" 3
[[ $(< "$TEST_ROOT/out") == '{ e0 = 1000; e1 = 3; e2 = 2000; e3 = 3; }' ]]
if ! jq -e 'has("gc")' "$TEST_ROOT/stats.json" > /dev/null; then
    [[ $(grep -c '^trace: x$' "$TEST_ROOT/err") == 2 ]]
fi

# Without --strict, no element runs in a job of its own: an element's job end
# would revert what it forced (a's outPath for JSON, a's type for XML), which
# a later element reads as it is.
pexpr='let t = "x" + "y"; a = { outPath = t; }; in builtins.seq a { inherit a; b = t; }'
expectJobs "$(jobsOf nix-instantiate --eval --json -E "$pexpr")" 2
[[ $(< "$TEST_ROOT/out") == '{"a":"xy","b":"xy"}' ]]
expectJobs "$(jobsOf nix-instantiate --eval --xml -E "${pexpr/outPath/type}")" 2
[[ $(grep -c '<string value="xy" />' "$TEST_ROOT/out") == 2 ]]

# A trace keeps nothing: re-forcing the cell whose evaluation showed it may
# show it again, and shows no other trace.
nix eval --expr 'let s = builtins.trace "s" { x = 1; }; in { a = s; b = s; }' 2> "$TEST_ROOT/err"
[[ $(grep '^trace: ' "$TEST_ROOT/err" | sort -u) == 'trace: s' ]]

# An effect, here a filtered copy, which nothing remembers, keeps the cell
# whose evaluation ran it: a later element that reads the cell does not run
# it again (the filter traces once per file, as when one element reads it),
# and the job is not kept whole.
for d in src src2; do
    mkdir -p "$TEST_ROOT/$d"
    for f in a b c d e; do echo "$d $f" > "$TEST_ROOT/$d/$f"; done
done
copy() {
    echo "let p = builtins.path { path = $TEST_ROOT/$1; filter = _: _: builtins.trace \"f\" true; }; in"
}
nix eval --impure --expr "$(copy src2) { a = builtins.seq p 1; }" 2> "$TEST_ROOT/err1" > /dev/null
jobsOf nix eval --impure --expr "$(copy src) { a = builtins.seq p 1; b = builtins.seq p 2; q = { x = 1; }; }" 2> "$TEST_ROOT/err" > /dev/null
[[ $(grep -c '^trace: f$' "$TEST_ROOT/err") == $(grep -c '^trace: f$' "$TEST_ROOT/err1") ]]
grepQuiet '^trace: f$' "$TEST_ROOT/err"
# unforce-always-revert keeps the effect's cell too: the copy runs once.
nix eval --impure --option unforce-always-revert true --expr "$(copy src) { a = builtins.seq p 1; b = builtins.seq p 2; }" 2> "$TEST_ROOT/err" > /dev/null
[[ $(grep -c '^trace: f$' "$TEST_ROOT/err") == $(grep -c '^trace: f$' "$TEST_ROOT/err1") ]]
if [[ $system == *-linux && $system != i686-linux ]] && ! jq -e 'has("gc")' "$TEST_ROOT/stats.json" > /dev/null; then
    [[ $(jq -r '.unforce.jobsKeptWhole' "$TEST_ROOT/stats.json") == 0 ]]
fi

# An element reads what an earlier element of its job forced through a trap,
# which records the read. c, kept for its filtered copy, holds q's list (both
# forced in e1) and, from e3, g's (forced in e2): the job's end keeps both,
# whose functions the next job's e5 compares.
hexpr="let big = n: builtins.length (builtins.genList (x: x) n); q = { k = [ (x: x) ]; }; g = { z = [ (x: x) ]; };
  c = let k = q.k; in builtins.seq (builtins.path { path = $TEST_ROOT/src; filter = _: _: true; }) (builtins.seq k { y = g.z; w = k; });
  in { e0 = big 300000; e1 = builtins.seq c 1; e2 = builtins.length g.z; e3 = builtins.length c.y; e4 = big 400000;
  e5 = [ (c.y == g.z) (c.w == q.k) ]; }"
[[ $(nix eval --impure --expr "$hexpr") == '{ e0 = 300000; e1 = 1; e2 = 1; e3 = 1; e4 = 400000; e5 = [ true true ]; }' ]]
# Sets and lists printed through traps are «repeated» as before, and the REPL
# prints an element's error and goes on.
sexpr='let big = n: builtins.length (builtins.genList (x: x) n); s = { x = [ (1 + 0) ]; }; l = [ s ];
  in { e0 = big 300000; e1 = s; e2 = s; e3 = l; e4 = l; e5 = { y = s; }; }'
[[ $(nix eval --expr "$sexpr") == '{ e0 = 300000; e1 = { x = [ 1 ]; }; e2 = «repeated»; e3 = [ «repeated» ]; e4 = «repeated»; e5 = { y = «repeated»; }; }' ]]
nix repl <<< ":p ${sexpr/e2 = s/e2 = throw \"boom\"}" | sed -e 's/\x1b\[[0-9;]*m//g' | tr -s ' \n' ' ' > "$TEST_ROOT/out"
[[ $(< "$TEST_ROOT/out") == '{ e0 = 300000; e1 = { x = [ 1 ]; }; e2 = «error: boom»; e3 = [ «repeated» ]; e4 = «repeated»; e5 = { y = «repeated»; }; } ' ]]
# Memory a job gave back is not in use: e1's job keeps p (forced again) above
# t's list, which it gives back, so e2 and e3 run in one job.
gexpr='let p = { y = builtins.genList (x: x) 10; z = 1; }; t = builtins.seq (builtins.genList (x: x) 300000) (builtins.length p.y);
  in { e0 = p.z; e1 = builtins.seq p.z t; e2 = builtins.length (builtins.genList (x: x) 1000); e3 = 1; }'
expectJobs "$(jobsOf nix eval --expr "$gexpr")" 3
[[ $(< "$TEST_ROOT/out") == '{ e0 = 1; e1 = 10; e2 = 1000; e3 = 1; }' ]]

# A job holds at most what the largest element took, and the element that
# crossed it: elements that allocate alike and keep nothing run in jobs of
# two after the first (counting what a job of several left as in use, each
# such job would let the next take one element more: 1, 2, 3, 4).
cexpr='let f = i: builtins.length (builtins.genList (x: x + i) 1000); in builtins.listToAttrs (builtins.genList (i: { name = "e${toString i}"; value = f i; }) 10)'
expectJobs "$(jobsOf nix eval --expr "$cexpr")" 6
[[ $(< "$TEST_ROOT/out") == '{ e0 = 1000; e1 = 1000; e2 = 1000; e3 = 1000; e4 = 1000; e5 = 1000; e6 = 1000; e7 = 1000; e8 = 1000; e9 = 1000; }' ]]

# flake check checks each output in a job of its own.
flakeDir=$TEST_ROOT/flake
mkdir -p "$flakeDir"
cp "${config_nix}" "$flakeDir/"
cat > "$flakeDir/flake.nix" <<EOF
{
  outputs = { self }: with import ./config.nix; {
    packages.$system.x = mkDerivation { name = "x"; builder = "/b"; };
    overlays.default = final: prev: { };
    hydraJobs.j = { y = mkDerivation { name = "y"; builder = "/b"; }; z = mkDerivation { name = "z"; builder = "/b"; }; };
  };
}
EOF
# A jobset's jobs run in its job: its loop iterates the jobset's set, which
# jobs of their own would free.
expectJobs "$(jobsOf nix flake check "$flakeDir")" 3
# flake show shows each package, and each jobset, in one.
expectJobs "$(jobsOf nix flake show "$flakeDir")" 2

# A derivation found in an earlier job is dropped in a later one; one job
# per attribute, then one per derivation found (its name, its drvPath); -qa
# keeps none of them, so one more element joins an open job.
cat > "$TEST_ROOT/drvs.nix" <<EOF
with import "${config_nix}";
rec {
  x = mkDerivation { name = "x"; builder = "/b"; };
  y = x;
  z = mkDerivation { name = "z"; builder = "/b"; };
}
EOF
expectJobs "$(jobsOf nix-env -f "$TEST_ROOT/drvs.nix" -qa)" 3
[[ $(< "$TEST_ROOT/out") == $'x\nz' ]]
# Derivations of merged elements that share their meta show it whole.
cat > "$TEST_ROOT/mdrvs.nix" <<EOF
with import "${config_nix}";
let m = { description = "d" + "x"; tags = [ ("t" + "1") ]; }; in rec {
  a = builtins.length (builtins.genList (x: x) 300000);
  a1 = mkDerivation { name = "a1"; builder = "/b"; meta = m; };
  a2 = mkDerivation { name = "a2"; builder = "/b"; meta = m; };
  a3 = a2;
  a4 = mkDerivation { name = "a4"; builder = "/b"; meta = m // { more = "y"; }; };
}
EOF
nix-env -f "$TEST_ROOT/mdrvs.nix" -qa --json --meta | jq -c '[keys, .a1.meta, .a2.meta, .a4.meta]' > "$TEST_ROOT/out"
[[ $(< "$TEST_ROOT/out") == '[["a1","a2","a4"],{"description":"dx","tags":["t1"]},{"description":"dx","tags":["t1"]},{"description":"dx","more":"y","tags":["t1"]}]' ]]
# Without unforcing (Boehm GC), --json queries after the walk: a's meta traces after b's walk.
echo "with import $config_nix; { a = mkDerivation { name = \"a\"; builder = \"/b\"; meta.m = builtins.trace \"q\" 1; }; b = builtins.trace \"w\" (mkDerivation { name = \"b\"; builder = \"/b\"; }); }" > "$TEST_ROOT/t.nix"
nix-env -f "$TEST_ROOT/t.nix" -qa --json --meta > /dev/null 2> "$TEST_ROOT/err"
if jq -e 'has("gc")' "$TEST_ROOT/stats.json" > /dev/null; then
    [[ $(< "$TEST_ROOT/err") == $'trace: w\ntrace: q' ]]
fi
# So in each of nix-env's channels (a directory of expressions).
mkdir -p "$TEST_ROOT/channels"
cp "$TEST_ROOT/drvs.nix" "$TEST_ROOT/channels/"
expectJobs "$(jobsOf nix-env -f "$TEST_ROOT/channels" -qa)" 3
[[ $(< "$TEST_ROOT/out") == $'x\nz' ]]
expectJobs "$(jobsOf nix-instantiate "$TEST_ROOT/drvs.nix")" 6
[[ $(wc -l < "$TEST_ROOT/out") == 2 ]]
# search visits each attribute in a job.
expectJobs "$(jobsOf nix search -f "$TEST_ROOT/drvs.nix" '' ^)" 2

if [[ $system == *-linux && $system != i686-linux ]] && ! jq -e 'has("gc")' "$TEST_ROOT/stats.json" > /dev/null; then
    # -qa --json queries each derivation in the job that found it, and -qaP reads only names and attribute paths
    # after the walk: neither holds the walk's jobs, so both keep less than -qa --system, which reads more.
    jobsOf nix-env -f "$TEST_ROOT/drvs.nix" -qa --system > /dev/null
    kept=$(jq .unforce.bytesKept "$TEST_ROOT/stats.json")
    for a in --json -P; do
        jobsOf nix-env -f "$TEST_ROOT/drvs.nix" -qa "$a" > /dev/null
        [[ $(jq .unforce.bytesKept "$TEST_ROOT/stats.json") -lt $kept ]]
    done

    # A store path that a reverted cell read or took cannot be deleted while the evaluator lives: a temporary root
    # holds it. The evaluation opens the fifo once the earlier elements' jobs ended, and reads it until it is closed.
    p=$(nix-store --add "$TEST_ROOT/src/a")
    q=$(nix-store --add "$TEST_ROOT/src/b")
    mkfifo "$TEST_ROOT/fifo"
    nix eval --impure --expr "[ \"\${builtins.path { path = $p; filter = _: _: true; }}\" (builtins.storePath $q) (builtins.fetchurl file://$TEST_ROOT/fifo) ]" > /dev/null &
    (exec 3> "$TEST_ROOT/fifo"; for x in "$p" "$q"; do if nix-store --delete "$x"; then fail "deleted $x while it was read"; fi; done)
    wait $!

    # A file that a job read must read the same until the evaluation ends, or it stops with exit status 102 in every
    # front end, through tryEval and nix eval's printer too; unchanged, the output is as before. q.a reads ./f in
    # vg1's job; vg2's blocks on a fifo while EDIT runs; vg3's re-forces q, whose a reads ./f again. The edit also
    # makes m a link to u/e, so that k, a link to m/../t, leads to u/t.
    vg=$TEST_ROOT/vg
    mkdir -p "$vg/t" "$vg/u/e"
    printf A > "$vg/t/x"
    ln -s m/../t "$vg/k"
    vgN=0
    vgRun() { # vgRun PROGRAM EDIT CMD...: CMD's exit status in vgRc, its output in $vg/out
        local fifo=$vg/fifo$((++vgN))
        mkfifo "$fifo"
        printf A > "$vg/f"
        rm -f "$vg/m"
        echo "with import $config_nix; let q = { a = builtins.readFile ./f; }; wait = builtins.fetchurl \"file://$fifo\";
          d = name: mkDerivation { inherit name; builder = \"/b\"; }; in $1" > "$vg/p.nix"
        "${@:3}" "$vg/p.nix" > "$vg/out" 2> "$vg/err" &
        (exec 3> "$fifo"; eval "$2")
        vgRc=0
        wait $! || vgRc=$?
    }
    vgCheck() { # vgCheck PROGRAM EXPECTED CMD...
        vgRun "$1" : "${@:3}"
        [[ $vgRc == 0 && $(< "$vg/out") == "$2" ]] || fail "unchanged: $vgRc $(< "$vg/out") $(< "$vg/err")"
        vgRun "$1" "printf B > $vg/f; ln -s $vg/u/e $vg/m" "${@:3}"
        [[ $vgRc == 102 && ! -s $vg/out ]] || fail "edited: $vgRc $(< "$vg/out") $(< "$vg/err")"
        grepQuiet "changed during evaluation" "$vg/err"
    }
    s='{ vg1 = q.a; vg2 = builtins.seq wait null; vg3 = (builtins.tryEval q.a).value; }'
    vgCheck "$s" '{ vg1 = "A"; vg2 = null; vg3 = "A"; }' nix eval --impure -f
    vgCheck "$s" '{"vg1":"A","vg2":null,"vg3":"A"}' nix eval --impure --json -f
    vgCheck "$s" '{ vg1 = "A"; vg2 = null; vg3 = "A"; }' nix-instantiate --eval --strict
    # vg3 reads ./f afresh itself.
    vgCheck '{ vg1 = builtins.seq q.a (d "vg1"); vg2 = builtins.seq wait (d "vg2"); vg3 = builtins.seq (builtins.readFile ./f) (d "vg3"); }' \
        $'vg1\nvg2\nvg3' nix-env -qa -f
    # So is a file that a store path's link leads to.
    mkdir "$vg/s"
    ln -s "$vg/f" "$vg/s/l"
    sp=$(nix-store --add "$vg/s")
    vgCheck "let r = { a = builtins.readFile $sp/l; }; in { vg1 = r.a; vg2 = builtins.seq wait null; vg3 = r.a; }" \
        '{"vg1":"A","vg2":null,"vg3":"A"}' nix eval --impure --json -f
    # And where a link leads, for a copy and a found path.
    xp=$(nix-store --add "$vg/t/x")
    vgCheck 'let r = { a = "${./k/x}"; }; in { vg1 = r.a; vg2 = builtins.seq wait null; vg3 = r.a; }' \
        "{\"vg1\":\"$xp\",\"vg2\":null,\"vg3\":\"$xp\"}" nix eval --impure --json -f
    vgCheck 'let r = { a = builtins.pathExists ./k/x; }; in { vg1 = r.a; vg2 = builtins.seq wait null; vg3 = r.a; }' \
        '{"vg1":true,"vg2":null,"vg3":true}' nix eval --impure --json -f

    # The REPL keeps the cell of every read, as stock keeps a forced thunk: r.a prints its first result after the edit
    # (a list: a string target is kept by copy anyway).
    replRun() { nix repl --file "$1" <<< $':p r\n:p builtins.seq wait null\n:p r'; }
    vgRun '{ r = { a = [ (builtins.readFile ./f) ]; }; inherit wait; }' "printf B > $vg/f" replRun
    [[ $vgRc == 0 && $(grep -c '"A"' "$vg/out") == 2 ]] || fail "repl: $vgRc $(< "$vg/out") $(< "$vg/err")"

    # A store path that the evaluation itself builds is no changed file: the cell that found it missing keeps its
    # value, and a new read finds it.
    o=$(nix eval --impure --raw --expr "(import $PWD/simple.nix).outPath")
    [[ $(nix eval --impure --json --expr "let c = { x = builtins.pathExists \"$o\"; }; in
      [ c.x (builtins.seq (builtins.readDir (import $PWD/simple.nix)) c.x) (builtins.pathExists \"$o\") ]") == '[false,false,true]' ]]
fi

# A mapAttrs attribute (a tAttrApp cell without Boehm GC) that forces itself
# fails at stock's call depth; one whose function's outer body recurses
# (`f name`) runs it a call level shallower than stock, so it fails one
# level later (two frames more), never sooner. With the profiler's hooks on,
# the cell is forced as stock forces it.
mapFrames() {
    expectStderr 1 nix-instantiate --eval --show-trace --max-call-depth 100 "$@" | grep -o '[0-9]* duplicate frames'
}
prof=(--option eval-profiler flamegraph --option eval-profile-file "$TEST_ROOT/prof")
# shellcheck disable=SC2016 # The ${} in these is Nix, not shell
self='let r = builtins.mapAttrs (n: v: r.${n}) { a = 1; }; in r.a'
outer='let g = k: g (k + 1); in (builtins.mapAttrs (n: g 0) { a = 1; }).a'
[[ $(mapFrames -E "$self") == '299 duplicate frames' ]]
[[ $(mapFrames "${prof[@]}" -E "$self") == '299 duplicate frames' ]]
[[ $(mapFrames "${prof[@]}" -E "$outer") == '195 duplicate frames' ]]
frames=197
[[ $(jobsOf nix-instantiate --eval -E 1) == - ]] && frames=195
[[ $(mapFrames -E "$outer") == "$frames duplicate frames" ]]
# An attribute made before the traversal and reverted at the end of the
# first element's job is forced again, in the next one, as it was made.
[[ $(nix eval --expr 'let m = builtins.mapAttrs (n: v: [ v n v ]) { a = 1; }; in builtins.seq m { x = builtins.length m.a; y = m.a; }') \
    == '{ x = 3; y = [ 1 "a" 1 ]; }' ]]

# An empty set that removeAttrs or null dynamic names leave is a set of its
# own, as before: printers show «repeated» only for one set shown twice.
# shellcheck disable=SC2016 # The ${} in this is Nix, not shell
[[ $(nix eval --expr 'let s = { a = 1; }; n = null; in [ (removeAttrs s [ "a" ]) (removeAttrs s [ "a" ]) { ${n} = 1; } { } ]') \
    == '[ { } { } { } { } ]' ]]
