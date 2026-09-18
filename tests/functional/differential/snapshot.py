#!/usr/bin/env python3
"""Normalisers for the differential harness.

  snapshot.py [--loose] pathinfo <path-info.json>   normalised JSON of `nix path-info --all --json --json-format 2`
  snapshot.py [--loose] db <db.sqlite>              deterministic dump of the store database
  snapshot.py renamed <db.sqlite> <real store dir>  the store up to a name-preserving renaming of store paths
  snapshot.py --strict renamed <db> <real store dir> the same, with build outputs' bytes digested rather than masked

Both replace the store directory (from $NIX_STORE_DIR) by "<store>" and drop
registrationTime, which is wall-clock.  Everything else is kept: narHash,
narSize, references, deriver, ca, ultimate, signatures, derivation outputs,
and the build trace.

With --loose, the contents of input-addressed store objects (those without a
content address, i.e. build outputs) are masked: their path is a function of
the derivation, and their bytes are the builder's business, which the
specification treats as a set of possible outcomes (01-specification.md,
section 2.6).  The strict view is still recorded beside the loose one.
"""
import json
import os
import sqlite3
import sys

STORE = os.environ.get("NIX_STORE_DIR", "/nix/store")
LOOSE = False
STRICT = False


def blank(v):
    if isinstance(v, str) and STORE and STORE in v:
        return v.replace(STORE, "<store>")
    return v


def sig(v):
    """A signature is `<key name>:<base64>`; the bytes depend on the signing key,
    which the test suite generates afresh in every run, so only the key name is
    comparable across runs."""
    name, sep, _ = v.partition(":")
    return name + sep + "<sig>" if sep else v


def sigs(v):
    return " ".join(sig(x) for x in v.split()) if isinstance(v, str) and v else v


def norm(o):
    if isinstance(o, dict):
        if LOOSE and "narHash" in o and o.get("ca") is None:
            o = dict(o, narHash="<output>", narSize=0)
        return {k: ("<store>" if k == "storeDir" else
                    [sig(x) for x in v] if k == "signatures" and isinstance(v, list) else
                    norm(v))
                for k, v in sorted(o.items()) if k != "registrationTime"}
    if isinstance(o, list):
        return [norm(x) for x in o]
    return blank(o)


def pathinfo(path):
    with open(path) as f:
        j = json.load(f)
    print(json.dumps(norm(j), sort_keys=True, indent=1))


def rows(cur, sql):
    for r in cur.execute(sql):
        print("  " + repr(tuple(blank(x) for x in r)))


def db(path):
    if not os.path.exists(path):
        print("(no database)")
        return
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    cur = con.cursor()
    tables = {r[0] for r in cur.execute("select name from sqlite_master where type='table'")}
    print("ValidPaths(path, hash, deriver, narSize, ultimate, sigs, ca):")
    for r in cur.execute("select path, hash, deriver, narSize, ultimate, sigs, ca from ValidPaths order by path"):
        r = list(r)
        r[5] = sigs(r[5])
        if LOOSE and r[6] is None:
            r[1], r[3] = "<output>", 0
        print("  " + repr(tuple(blank(x) for x in r)))
    print("Refs(referrer, reference):")
    rows(cur, "select a.path, b.path from Refs"
              " join ValidPaths a on a.id = Refs.referrer"
              " join ValidPaths b on b.id = Refs.reference order by a.path, b.path")
    print("DerivationOutputs(drv, id, path):")
    rows(cur, "select v.path, d.id, d.path from DerivationOutputs d"
              " join ValidPaths v on v.id = d.drv order by v.path, d.id")
    if "BuildTraceV3" in tables:
        print("BuildTraceV3(drvPath, outputName, outputPath, signatures):")
        for r in cur.execute("select drvPath, outputName, outputPath, signatures from BuildTraceV3"
                             " order by drvPath, outputName"):
            r = list(r)
            r[3] = sigs(r[3])
            print("  " + repr(tuple(blank(x) for x in r)))
    con.close()


def storedir(path):
    """Print the logical store directory a database was written for (the directory of
    its first valid path), or nothing for an absent or empty database.  A chroot store
    created with NIX_STORE_DIR unset has /nix/store here even though its files live under
    <root>/nix/store; a client must use the same logical directory to read it."""
    if not os.path.exists(path):
        return
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    row = con.execute("select path from ValidPaths limit 1").fetchone()
    con.close()
    if row:
        print(os.path.dirname(row[0]))


def listing(dbpath, realdir):
    """Sorted listing of the store directory.  Entries that are not valid paths
    (leftover scratch outputs, lock files of never-registered paths) are printed
    with their hash part masked in the loose view, because scratch names are
    random; their presence and name still count.  Suffixes .lock, .check and
    .chroot belong to the path they decorate."""
    import re
    valid = set()
    if os.path.exists(dbpath):
        con = sqlite3.connect(f"file:{dbpath}?mode=ro", uri=True)
        valid = {os.path.basename(r[0]) for r in con.execute("select path from ValidPaths")}
        con.close()
    try:
        entries = sorted(os.listdir(realdir))
    except FileNotFoundError:
        print("(no store directory)")
        return
    pat = re.compile(r"^([0-9a-df-np-sv-z]{32})-(.*)$")
    out = []
    for e in entries:
        # The store's own furniture, not its contents: the legacy links table
        # and the object store (doc/lazy-store/01-specification.md, 9.10).
        if e in (".links", ".objects"):
            continue
        base = re.sub(r"\.(lock|check|chroot)$", "", e)
        m = pat.match(e)
        if LOOSE and m and base not in valid:
            out.append("<unregistered>-" + m.group(2))
        else:
            out.append(e)
    for line in sorted(out):
        print(line)


def renamed(dbpath, realdir):
    """One line per valid store object, sorted: the object's name, a digest of
    its tree in which the hash part of every embedded store path is masked,
    whether it is content-addressed, its references' names and its deriver's
    name.  Two stores agree here exactly when they hold the same objects up to
    a renaming of store paths that keeps names: the view for a run whose
    naming function differs from the reference's (01-specification.md,
    section 9.9), which the path-comparing views cannot read.  Input-addressed
    objects are masked as in the loose view, unless --strict, which digests
    their bytes too, so a builder that writes the same bytes up to the renaming
    reads equal and one that does not reads different.  A dropped
    input-addressed object still differs by name, reference or deriver.  A
    derivation is digested through its ATerm with the two path-sorted lists
    re-sorted after the masking (canonical_drv below), since a renaming can
    change the order in which the format lists its inputs."""
    import collections
    import hashlib
    import re
    import stat
    if not os.path.exists(dbpath):
        print("(no database)")
        return
    con = sqlite3.connect(f"file:{dbpath}?mode=ro", uri=True)
    paths = {r[0]: (r[1], r[2], r[3]) for r in con.execute("select id, path, ca, deriver from ValidPaths")}
    refs = collections.defaultdict(list)
    for a, b in con.execute("select referrer, reference from Refs"):
        if a in paths and b in paths:
            refs[a].append(paths[b][0])
    con.close()
    storeb = STORE.encode()
    hashre = re.compile(re.escape(storeb) + rb"/[0-9a-df-np-sv-z]{32}-")

    def mask(data):
        return hashre.sub(storeb + b"/<hash>-", data)

    def name_of(p):
        base = os.path.basename(p)
        return base[33:] if len(base) > 33 and base[32] == "-" else base

    def canonical_drv(data):
        """A derivation's ATerm with every string masked and its two lists that
        the format sorts by store path (inputDrvs, inputSrcs) re-sorted after the
        masking, so that a renaming that changes the order of the hash parts
        leaves the digest alone.  Returns None when the text is not an ATerm
        derivation, and the caller hashes the masked bytes instead."""
        text = data.decode("utf-8", "surrogateescape")
        pos = 0

        def parse():
            nonlocal pos
            c = text[pos]
            if c == '"':
                pos += 1
                start = pos
                while text[pos] != '"':
                    pos += 2 if text[pos] == "\\" else 1
                value = text[start:pos]
                pos += 1
                return ("s", mask(value.encode("utf-8", "surrogateescape")).decode("utf-8", "surrogateescape"))
            if c in "[(":
                close = "]" if c == "[" else ")"
                pos += 1
                items = []
                while text[pos] != close:
                    items.append(parse())
                    if text[pos] == ",":
                        pos += 1
                pos += 1
                return ("l" if c == "[" else "t", items)
            start = pos
            while pos < len(text) and (text[pos].isalnum() or text[pos] == "_"):
                pos += 1
            if pos == start:
                raise ValueError("not an ATerm")
            return ("f", text[start:pos], parse())

        try:
            tree = parse()
        except (ValueError, IndexError):
            return None
        if text[pos:].strip():
            return None

        def canon(node):
            kind = node[0]
            if kind == "f":
                name, args = node[1], node[2]
                if name == "Derive" and args[0] == "t" and len(args[1]) >= 3:
                    fields = list(args[1])
                    fields[1] = ("l", sorted((canon(x) for x in fields[1][1]), key=repr))
                    fields[2] = ("l", sorted((canon(x) for x in fields[2][1]), key=repr))
                    return ("f", name, ("t", [canon(fields[0])] + fields[1:3] + [canon(x) for x in fields[3:]]))
                return ("f", name, canon(args))
            if kind in "lt":
                return (kind, [canon(x) for x in node[1]])
            return node

        return repr(canon(tree)).encode("utf-8", "surrogateescape")

    def digest(real):
        h = hashlib.sha256()

        def put(tag, data):
            h.update(tag + len(data).to_bytes(8, "little") + data)

        def walk(p):
            st = os.lstat(p)
            if stat.S_ISDIR(st.st_mode):
                h.update(b"d(")
                for e in sorted(os.listdir(p)):
                    put(b"n", e.encode())
                    walk(os.path.join(p, e))
                h.update(b")")
            elif stat.S_ISLNK(st.st_mode):
                put(b"l", mask(os.readlink(p).encode()))
            elif stat.S_ISREG(st.st_mode):
                with open(p, "rb") as f:
                    data = f.read()
                canonical = canonical_drv(data) if p == real and p.endswith(".drv") else None
                put(b"D" if canonical is not None else b"x" if st.st_mode & 0o111 else b"f",
                    canonical if canonical is not None else mask(data))
            else:
                h.update(b"?")

        walk(real)
        return h.hexdigest()

    lines = []
    for id_, (path, ca, deriver) in paths.items():
        real = os.path.join(realdir, os.path.basename(path))
        if ca is None and not STRICT:
            d = "<output>"
        elif not os.path.lexists(real):
            d = "<missing>"
        else:
            d = digest(real)
        lines.append("\t".join([
            name_of(path), d, "ca" if ca else "output",
            ",".join(sorted(name_of(r) for r in refs[id_])) or "-",
            name_of(deriver) if deriver else "-"]))
    for line in sorted(lines):
        print(line)


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--loose":
        LOOSE = True
        args = args[1:]
    if args and args[0] == "--strict":
        STRICT = True
        args = args[1:]
    if args[0] == "listing":
        listing(args[1], args[2])
    elif args[0] == "renamed":
        renamed(args[1], args[2])
    else:
        {"pathinfo": pathinfo, "db": db, "storedir": storedir}[args[0]](args[1])
