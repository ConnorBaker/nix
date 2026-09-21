#!/usr/bin/env python3
"""(C2) observer: a second process reading a command's stdout.

Usage: <command> | c2-observe.py <report-file>

Every store path that appears on a line is checked, at the moment this
process reads the line and before the line is forwarded, for being in the
store: registered as valid in the store's database when NIX_STATE_DIR names
one (the daemon's clients see the same database), and otherwise merely
present on the filesystem, which is a proxy the report labels as such.  The
report has one line per distinct store object:
    <store path> present|absent
Under the specification's (C2) an object in Delta must be present; an object
outside Delta (a mounted flake input master prints without copying) may be
absent on master too, so the differential use is: the implementation's report
must never say "absent" where the reference's says "present".
"""
import os
import re
import sys

store = os.environ.get("NIX_STORE_DIR", "/nix/store")
pat = re.compile(re.escape(store) + r"/([0-9a-df-np-sv-z]{32}-[^\s\"'`:;,)]+)")

db_path = os.path.join(os.environ.get("NIX_STATE_DIR", ""), "db", "db.sqlite") if os.environ.get("NIX_STATE_DIR") else None
db = None
checks = set()


def in_store(root):
    """Valid in the database if there is one by now (the command creates it
    on its first store access), else present on the filesystem."""
    global db
    if db is None and db_path and os.path.exists(db_path):
        import sqlite3
        db = sqlite3.connect("file:" + db_path + "?mode=ro", uri=True)
    if db is not None:
        try:
            row = db.execute("select 1 from ValidPaths where path = ?", (root,)).fetchone()
            checks.add("valid in " + db_path)
            return row is not None
        except Exception as e:  # locked or mid-schema: fall back for this one check
            checks.add("filesystem presence (database unavailable: %s)" % e)
    else:
        checks.add("filesystem presence (proxy: no database yet)")
    return os.path.lexists(root)


seen = {}
with open(sys.argv[1], "w") as report:
    for line in sys.stdin:
        for m in pat.finditer(line):
            root = store + "/" + m.group(1).split("/")[0].rstrip(".,")
            if root in seen:
                continue
            seen[root] = in_store(root)
            report.write(f"{root} {'present' if seen[root] else 'absent'}\n")
            report.flush()
        sys.stdout.write(line)
        sys.stdout.flush()
    for check in sorted(checks):
        report.write("# checked by: " + check + "\n")
