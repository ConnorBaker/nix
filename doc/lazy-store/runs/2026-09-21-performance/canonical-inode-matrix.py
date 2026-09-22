"""Finite relational check of the production SQL plus its minimum fold.

The C++ lookup tests separately exercise the implementation of that fold.
"""
import ast
import itertools
import json
from pathlib import Path
import re
import sqlite3

source = (Path(__file__).resolve().parents[4] / "src/libstore/local-store.cc").read_text()


def cpp_strings(text):
    return "".join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', text))


query = cpp_strings(re.search(r'QueryBlobInodes\.create\(\s*state->db,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\);', source)[1])
lookup = cpp_strings(re.search(r"static constexpr auto lookupBlobSql =(.*?);\s*\n", source, re.S)[1])
c = sqlite3.connect(":memory:")
c.executescript("""
CREATE TABLE ValidPaths(path TEXT PRIMARY KEY);
CREATE TABLE Ingestions(id INTEGER PRIMARY KEY,path TEXT);
CREATE TABLE Blobs(id BLOB,exec INTEGER,ino INTEGER,dev INTEGER,ingestion INTEGER,
                   name TEXT,size INTEGER,PRIMARY KEY(id,exec,ingestion));
CREATE INDEX IndexBlobsIngestion ON Blobs(ingestion);
""")
initial = (-(1 << 63), -(1 << 63), 0)
# Ordered physical classes include negative SQLite values and both signed minima.
classes = [(-(1 << 63), -(1 << 63)), (-1, 7), (0, -(1 << 63))]


def check(states, ranks):
    c.executescript("DELETE FROM Blobs; DELETE FROM Ingestions; DELETE FROM ValidPaths;")
    expected = []
    registered = [[], []]
    for executable in range(2):
        for ordinal in range(3):
            state = states[executable * 3 + ordinal]
            ingestion = executable * 3 + ordinal + 1
            if not state:
                continue
            path = None if state == 1 else str(ingestion)
            dev, ino = classes[ranks[executable][ordinal]]
            c.execute("INSERT INTO Ingestions VALUES(?,?)", (ingestion, path))
            c.execute("INSERT INTO Blobs VALUES(?,?,?,?,?,?,?)",
                      (b"blob", executable, ino, dev, ingestion, "", 1))
            if state == 3:
                c.execute("INSERT INTO ValidPaths VALUES(?)", (path,))
                registered[executable].append((dev, ino, ingestion))
        if registered[executable]:
            expected.append((min(registered[executable])[1], b"blob"))
    actual = c.execute(query).fetchall()
    assert sorted(actual) == sorted(expected), (states, ranks, actual, expected)
    # Enumerate even without deleting rejected rows, as a read-only store does.
    for executable in range(2):
        cursor = initial
        observed = []
        while row := min(c.execute(lookup, (b"blob", executable, *cursor)),
                         key=lambda row: (row[1], row[0], row[5]), default=None):
            next_cursor = (row[1], row[0], row[5])
            assert next_cursor > cursor
            observed.append(next_cursor)
            cursor = next_cursor
        assert observed == sorted(registered[executable]), (states, ranks, observed)


def main():
    visibility_cases = 0
    for states in itertools.product(range(4), repeat=6):
        check(states, ((2, 0, 1), (0, 2, 1)))
        visibility_cases += 1
    # All 13 ordered partitions of three candidates into physical classes.
    ordered_partitions = [ranks for ranks in itertools.product(range(3), repeat=3)
                          if set(ranks) == set(range(max(ranks) + 1))]
    assert len(ordered_partitions) == 13
    class_cases = 0
    for executable in range(2):
        for ranks in ordered_partitions:
            for visible in itertools.product(range(4), repeat=3):
                states = [3] * 6
                states[executable * 3:executable * 3 + 3] = visible
                physical = [(2, 1, 0), (2, 1, 0)]
                physical[executable] = ranks
                check(states, physical)
                class_cases += 1
    print(json.dumps({
        "sqlite": sqlite3.sqlite_version,
        "query_provenance": "src/libstore/local-store.cc: QueryBlobInodes and lookupBlobSql, extracted verbatim; streamed lookup rows reduced to their signed (dev, ino, ingestion) minimum",
        "visibility_domain": "3 candidates x 2 executable classes; each absent/unnamed/named-invalid/registered",
        "visibility_cases": visibility_cases,
        "physical_class_domain": "2 executable classes x all 13 ordered three-candidate partitions x 64 visibility states; other mode independently present",
        "physical_class_cases": class_cases,
        "cases": visibility_cases + class_cases,
        "failures": 0,
        "query": query,
        "lookup": lookup,
        "explain": c.execute("EXPLAIN QUERY PLAN " + query).fetchall(),
        "lookup_explain": c.execute("EXPLAIN QUERY PLAN " + lookup, (b"blob", 0, *initial)).fetchall(),
    }, indent=2))


if __name__ == "__main__":
    main()
