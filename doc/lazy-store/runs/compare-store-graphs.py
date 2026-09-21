#!/usr/bin/env python3
"""Compare every registered object and reference edge, allowing address changes.

Usage: compare-store-graphs.py BASE_ROOT BRANCH_ROOT OUTPUT_JSON
Roots are private stores containing nix/store and nix/var/nix/db/db.sqlite.
The coarse, name-preserving digest only constructs a candidate bijection.
Verification then rewrites actual addresses, including declared derivation
outputs, and compares every object's bytes, tree shape and executable bits.
Ambiguous candidate mappings are refused rather than assumed equivalent.
"""
import collections
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import sqlite3
import sys

spec = importlib.util.spec_from_file_location(
    "snapshot", Path(__file__).resolve().parents[3] / "tests/functional/differential/snapshot.py")
snapshot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(snapshot)
snapshot.STRICT = True


def objects(root, rewrite=None):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        snapshot.renamed(root / "nix/var/nix/db/db.sqlite", root / "nix/store",
                         include_paths=True, rewrite=rewrite)
    result = {}
    for line in out.getvalue().splitlines():
        path, digest, ca, refs, deriver = line.split("\t")
        assert digest != "<missing>", path
        result[path] = (path.rsplit("/", 1)[1][33:], digest, ca, deriver)
    return result


def graph(root):
    nodes = objects(root)
    with sqlite3.connect(f"file:{root}/nix/var/nix/db/db.sqlite?mode=ro", uri=True) as db:
        edges = set(db.execute("select a.path,b.path from Refs r "
                               "join ValidPaths a on a.id=r.referrer "
                               "join ValidPaths b on b.id=r.reference"))
        outputs = {(drv, name): path for drv, name, path in db.execute(
            "select v.path,d.id,d.path from DerivationOutputs d join ValidPaths v on v.id=d.drv")}
        derivers = dict(db.execute("select path,deriver from ValidPaths where deriver is not null"))
    outgoing, incoming = collections.defaultdict(list), collections.defaultdict(list)
    for a, b in edges:
        outgoing[a].append(b)
        incoming[b].append(a)
    return nodes, edges, outgoing, incoming, outputs, derivers


def colour(value):
    return hashlib.sha256(repr(value).encode()).hexdigest()


def compare(base, branch):
    graphs = [graph(base), graph(branch)]
    colours = [{k: colour(v) for k, v in g[0].items()} for g in graphs]
    previous = 0
    while True:
        assert collections.Counter(colours[0].values()) == collections.Counter(colours[1].values())
        distinct = len(set(colours[0].values()))
        if distinct == previous:
            break
        previous = distinct
        colours = [{k: colour((v, tuple(sorted(cs[x] for x in g[2][k])),
                                  tuple(sorted(cs[x] for x in g[3][k]))))
                    for k, v in cs.items()} for g, cs in zip(graphs, colours)]
    cells = [collections.defaultdict(list), collections.defaultdict(list)]
    for cell, cs in zip(cells, colours):
        for path, c in cs.items():
            cell[c].append(path)
    assert all(len(v) == 1 for cell in cells for v in cell.values()), "ambiguous bijection"
    mapping = {v[0]: cells[1][c][0] for c, v in cells[0].items()}
    assert {(mapping[a], mapping[b]) for a, b in graphs[0][1]} == graphs[1][1]
    assert {(mapping[p], output) for p, output in graphs[0][4]} == set(graphs[1][4])
    addresses = dict(mapping)
    for (drv, output), path in graphs[0][4].items():
        other = graphs[1][4][mapping[drv], output]
        assert addresses.setdefault(path, other) == other
    assert {mapping[p]: addresses.get(d, d) for p, d in graphs[0][5].items()} == graphs[1][5]
    prefixes = {}
    for before, after in addresses.items():
        a, b = before.rsplit("/", 1)[1], after.rsplit("/", 1)[1]
        assert a[33:] == b[33:]
        assert prefixes.setdefault(a[:32].encode(), b[:32].encode()) == b[:32].encode()
    pattern = re.compile(rb"(/nix/store/)([0-9a-df-np-sv-z]{32})(-)")

    def rewrite(data):
        return pattern.sub(lambda m: m[1] + prefixes.get(m[2], m[2]) + m[3], data)

    exact_base = objects(base, rewrite)
    exact_branch = objects(branch, lambda x: x)
    mismatches = [(a, b) for a, b in mapping.items() if exact_base[a] != exact_branch[b]]
    assert not mismatches, f"object contents differ after address rewrite: {mismatches[:8]}"
    return dict(paths=len(mapping), edges=len(graphs[0][1]),
                derivation_outputs=len(graphs[0][4]),
                exact_contents_after_address_rewrite=True, mapping=mapping)


if __name__ == "__main__":
    result = compare(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
    Path(sys.argv[3]).write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "mapping"}))
