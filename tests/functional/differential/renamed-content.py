#!/usr/bin/env python3
"""Compare two runs' stores up to a name-preserving renaming of store paths.

Usage: renamed-content.py [--strict] OUT_DIR LABEL_A LABEL_B SUITE [TEST...]

Reads the `snap-*.renamed` side files written by lib.sh (snapshot.py renamed):
one line per object with its name, the digest of its tree after the hash part
of every embedded store path is masked, whether it is content-addressed, its
references' names and its deriver's name.  Two runs agree here exactly when
they registered the same multiset of such lines in every store snapshot: the
same objects, with the same bytes up to the renaming, in the same reference
graph by name.  This is the check for a run whose naming function differs
from the reference's (01-specification.md, section 9.9): every source path
changes, so the path-comparing views cannot read it, and the name multiset
alone cannot see content.  Stronger than name-multiset.py, weaker than SAME.
With --strict the `.renamed-strict` files are compared, in which build
outputs' bytes are digested rather than masked (verdicts RENAMED-STRICT-*).

With no TEST arguments, every test present in both runs is compared.
"""
import collections
import os
import sys


def load(path):
    with open(path) as f:
        lines = f.read().splitlines()
    if any(line.startswith("SNAPSHOT-ERROR") for line in lines):
        return None
    return collections.Counter(lines)


def main(argv):
    strict = False
    if len(argv) > 1 and argv[1] == "--strict":
        strict = True
        argv = argv[:1] + argv[2:]
    if len(argv) < 5:
        sys.stderr.write(__doc__)
        return 2
    out, a, b, suite = argv[1:5]
    tests = argv[5:]
    suffix = ".renamed-strict" if strict else ".renamed"
    label = "RENAMED-STRICT" if strict else "RENAMED"
    if not tests:
        da = set(os.listdir(os.path.join(out, a, suite))) if os.path.isdir(os.path.join(out, a, suite)) else set()
        db = set(os.listdir(os.path.join(out, b, suite))) if os.path.isdir(os.path.join(out, b, suite)) else set()
        tests = sorted(da & db)
    rc = 0
    for test in tests:
        pa = os.path.join(out, a, suite, test)
        pb = os.path.join(out, b, suite, test)
        files = sorted({f for d in (pa, pb) if os.path.isdir(d) for f in os.listdir(d) if f.endswith(suffix)})
        if not files:
            print(f"{suite}\t{test}\tNO-{label}-VIEW")
            continue
        verdict = f"{label}-EQUAL"
        detail = []
        objects = 0
        for f in files:
            fa, fb = os.path.join(pa, f), os.path.join(pb, f)
            if not (os.path.exists(fa) and os.path.exists(fb)):
                verdict = f"{label}-DIFFER"
                detail.append(f"missing:{f}")
                continue
            ca, cb = load(fa), load(fb)
            if ca is None or cb is None:
                verdict = f"{label}-DIFFER"
                detail.append(f"unreadable:{f}")
                continue
            objects += sum(ca.values())
            if ca != cb:
                verdict = f"{label}-DIFFER"
                detail.append(f"differs:{f}")
                for line in sorted(set(ca) | set(cb)):
                    if ca[line] != cb[line]:
                        detail.append(f"\t\t{ca[line]}/{cb[line]}\t{line}")
        if verdict != f"{label}-EQUAL":
            rc = 1
        print(f"{suite}\t{test}\t{verdict}\t{objects}" + ("\n" + "\n".join(detail) if detail else ""))
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
