#!/usr/bin/env python3
"""Compare the multiset of store-object *names* (hash prefix stripped) between two runs.

Usage: name-multiset.py OUT_DIR LABEL_A LABEL_B SUITE [TEST...]

The loose and strict snapshot views compare full store paths, so a test whose
inputs are nondeterministic (tarballs with timestamps, profiles embedding the
test root) shows as DIFF even when both sides wrote the same objects.  This
view is invariant under such hash churn: two runs agree here exactly when they
registered the same number of objects of each name.  It is weaker than the
snapshot views (it cannot see content or reference differences), so it is a
strengthening of a by-construction exclusion, not a replacement for SAME.

With no TEST arguments, every test present in both runs is compared.
"""
import collections
import json
import os
import sys


def names(path):
    with open(path) as f:
        try:
            d = json.load(f)
        except json.JSONDecodeError:
            return None
    c = collections.Counter()
    for k in d["info"].keys():
        c[k[33:] if len(k) > 33 and k[32] == "-" else k] += 1
    return c


def main(argv):
    if len(argv) < 5:
        sys.stderr.write(__doc__)
        return 2
    out, a, b, suite = argv[1:5]
    tests = argv[5:]
    if not tests:
        da = set(os.listdir(os.path.join(out, a, suite))) if os.path.isdir(os.path.join(out, a, suite)) else set()
        db = set(os.listdir(os.path.join(out, b, suite))) if os.path.isdir(os.path.join(out, b, suite)) else set()
        tests = sorted(da & db)
    rc = 0
    for test in tests:
        pa = os.path.join(out, a, suite, test, "snap-final.main.pathinfo.json")
        pb = os.path.join(out, b, suite, test, "snap-final.main.pathinfo.json")
        if not (os.path.exists(pa) and os.path.exists(pb)):
            print(f"{suite}\t{test}\tNO-SNAPSHOT")
            continue
        na, nb = names(pa), names(pb)
        if na is None or nb is None:
            print(f"{suite}\t{test}\tUNREADABLE-SNAPSHOT")
            continue
        if na == nb:
            print(f"{suite}\t{test}\tNAMES-EQUAL\t{sum(na.values())}\t{len(na)}")
        else:
            rc = 1
            print(f"{suite}\t{test}\tNAMES-DIFFER")
            for n in sorted(set(na) | set(nb)):
                if na[n] != nb[n]:
                    print(f"\t\t{n}\t{na[n]}\t{nb[n]}")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
