#!/usr/bin/env python3
"""Compare every registered object and reference edge, allowing address changes.

Usage: compare-store-graphs.py BASE_ROOT BRANCH_ROOT OUTPUT_JSON
Roots are private stores containing nix/store and nix/var/nix/db/db.sqlite.
The coarse, name-preserving digest only constructs a candidate bijection.
Verification then rewrites actual addresses, including declared derivation
outputs, and compares every object's bytes, tree shape and executable bits.
Address substitution applies inside opaque strings, including attached compiler
options; it does not interpret the semantics of embedded programs. Only complete
mapped store basenames are substituted, never another or longer basename.
Ambiguous candidate mappings are refused rather than assumed equivalent.
Pass --benchmark-output DIR --scheme SCHEME to also compare every paired
JSON result from these exact store roots, binding each returned drvPath to
the verified graph mapping. Compare each cold store pair separately.
"""
import argparse
import collections
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import sqlite3
import stat

spec = importlib.util.spec_from_file_location(
    "snapshot", Path(__file__).resolve().parents[3] / "tests/functional/differential/snapshot.py")
snapshot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(snapshot)
snapshot.STRICT = True
snapshot.STORE = "/nix/store"


def executable_modes(path):
    """NAR executability is the owner's bit, including for parsed derivations."""
    result = []

    def walk(node, relative):
        mode = node.lstat().st_mode
        if stat.S_ISDIR(mode):
            for child in sorted(node.iterdir()):
                walk(child, relative / child.name)
        elif stat.S_ISREG(mode):
            result.append((str(relative), bool(mode & stat.S_IXUSR)))
        else:
            assert stat.S_ISLNK(mode), f"unsupported store node: {node}"

    walk(path, Path("."))
    return tuple(result)


def objects(root, rewrite=None):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        snapshot.renamed(root / "nix/var/nix/db/db.sqlite", root / "nix/store",
                         include_paths=True, rewrite=rewrite)
    result = {}
    for line in out.getvalue().splitlines():
        path, digest, ca, refs, deriver = line.split("\t")
        assert digest != "<missing>", path
        result[path] = (path.rsplit("/", 1)[1][33:], digest, ca, deriver,
                        executable_modes(root / "nix/store" / Path(path).name))
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


def address_rewriter(addresses):
    encoded = {before.encode(): after.encode() for before, after in addresses.items()}
    # Strings are opaque: an address may follow any byte, for example in
    # an attached compiler option. The quotient substitutes the same known
    # address in every context. Consume the complete StorePath::checkName
    # basename so another or longer name remains a distinct literal.
    pattern = re.compile(rb"/nix/store/[0-9a-df-np-sv-z]{32}-[A-Za-z0-9+._?=-]+")
    return lambda data: pattern.sub(lambda m: encoded.get(m[0], m[0]), data)


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
    assert len(set(addresses.values())) == len(addresses), "declared output addresses are not injective"
    assert {mapping[p]: addresses.get(d, d) for p, d in graphs[0][5].items()} == graphs[1][5]
    for before, after in addresses.items():
        assert Path(before).name[33:] == Path(after).name[33:]
    exact_base = objects(base, address_rewriter(addresses))
    exact_branch = objects(branch, lambda x: x)
    mismatches = [(a, b) for a, b in mapping.items() if exact_base[a] != exact_branch[b]]
    assert not mismatches, f"object contents differ after address rewrite: {mismatches[:8]}"
    return dict(paths=len(mapping), edges=len(graphs[0][1]),
                derivation_outputs=len(graphs[0][4]),
                exact_contents_after_address_rewrite=True, mapping=mapping)


def compare_outputs(mapping, directory, scheme, *, roots=None):
    """Check every actual result, including which derivation each attribute returned."""
    pairs = collections.defaultdict(dict)
    rows = [json.loads(line) for line in (directory / "samples.jsonl").read_text().splitlines()]
    for row in rows:
        if row["scheme"] != scheme:
            continue
        assert row["returncode"] == 0, row["id"]
        assert row["label"] in ("base", "branch"), row["label"]
        if roots is not None:
            command = row["command"]
            assert command.count("--store") == 1, row["id"]
            store = command[command.index("--store") + 1]
            if store != f"local?root={roots[row['label']]}":
                continue
        key = row["phase"], row["attributes"], row["trial"]
        assert row["label"] not in pairs[key], f"duplicate sample: {row['id']}"
        output = json.loads((directory / "logs" / f"{row['id']}.json").read_text())
        assert isinstance(output, list) and len(output) == row["attributes"], row["id"]
        pairs[key][row["label"]] = output
    assert pairs, f"no samples for {scheme}"
    for key, pair in pairs.items():
        assert set(pair) == {"base", "branch"}, f"unpaired sample: {key}"
        base = [dict(item, drvPath=mapping[item["drvPath"]]) for item in pair["base"]]
        assert base == pair["branch"], f"evaluation output differs after address rewrite: {scheme} {key}"
    return dict(pairs=len(pairs), samples=2 * len(pairs), exact_outputs_after_address_rewrite=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_root", type=Path)
    parser.add_argument("branch_root", type=Path)
    parser.add_argument("output_json", type=Path)
    parser.add_argument("--benchmark-output", type=Path)
    parser.add_argument("--scheme", choices=("path", "git", "github"))
    args = parser.parse_args()
    if bool(args.benchmark_output) != bool(args.scheme):
        parser.error("--benchmark-output and --scheme must be supplied together")
    result = compare(args.base_root.resolve(), args.branch_root.resolve())
    if args.benchmark_output:
        result["evaluation_outputs"] = compare_outputs(
            result["mapping"], args.benchmark_output.resolve(), args.scheme,
            roots={"base": args.base_root.resolve(), "branch": args.branch_root.resolve()})
    args.output_json.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "mapping"}))
