"""`db-inspect` subcommand — post-mortem analyses of the eval-trace SQLite DB (F1-F8)."""

from __future__ import annotations

import sqlite3
from collections.abc import Iterable
from pathlib import Path

from cyclopts import App
from rich.console import Console
from rich.table import Table

from ..cliutil import DEFAULT_NIX_CACHE
from ..render import export_console, fmt_int, pct, section, simple_table

TRACE_TABLES = (
    "Strings",
    "DataPaths",
    "Results",
    "DepKeySets",
    "Traces",
    "Sessions",
    "History",
    "SessionRuntimeRoots",
    "DirSets",
)


def _open(path: Path) -> sqlite3.Connection:
    if not path.exists():
        raise SystemExit(f"DB not found: {path}")
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row
    return con


def _resolve_attr_path(vocab: sqlite3.Connection, attr_path_id: int) -> str:
    """Walk `AttrPaths` / `AttrNames` from a leaf id up to the root."""
    if attr_path_id <= 0:
        return "<root>"
    names: list[str] = []
    current = attr_path_id
    seen: set[int] = set()
    while current > 0:
        if current in seen:
            break
        seen.add(current)
        row = vocab.execute(
            "SELECT parent, child FROM AttrPaths WHERE id = ?", (current,)
        ).fetchone()
        if row is None:
            break
        parent_id: int = int(row["parent"])
        child_id: int = int(row["child"])
        name_row = vocab.execute("SELECT name FROM AttrNames WHERE id = ?", (child_id,)).fetchone()
        names.append(str(name_row["name"]) if name_row else f"<{child_id}>")
        current = parent_id
    return ".".join(reversed([n for n in names if n]))


def _sizes(n: int) -> str:
    if n < 1024:
        return f"{n} B"
    if n < 1024 * 1024:
        return f"{n / 1024:.1f} KiB"
    if n < 1024 * 1024 * 1024:
        return f"{n / (1024 * 1024):.1f} MiB"
    return f"{n / (1024 * 1024 * 1024):.2f} GiB"


def _histogram(values: Iterable[int]) -> list[tuple[str, int, int]]:
    """Return [(label, count, sum_bytes), ...] binned by size."""
    bins: list[tuple[str, int, int]] = [
        ("< 1 KiB", 0, 1 << 10),
        ("1 KiB - 10 KiB", 1 << 10, 10 << 10),
        ("10 KiB - 100 KiB", 10 << 10, 100 << 10),
        ("100 KiB - 1 MiB", 100 << 10, 1 << 20),
        ("1 MiB - 10 MiB", 1 << 20, 10 << 20),
        ("> 10 MiB", 10 << 20, 1 << 62),
    ]
    counts = [0] * len(bins)
    totals = [0] * len(bins)
    for v in values:
        for i, (_, lo, hi) in enumerate(bins):
            if lo <= v < hi:
                counts[i] += 1
                totals[i] += v
                break
    return [(lbl, counts[i], totals[i]) for i, (lbl, _, _) in enumerate(bins)]


def _report_tables(console: Console, con: sqlite3.Connection) -> None:
    section(console, "F1: table row counts")
    rows: list[list[str]] = []
    for tbl in TRACE_TABLES:
        try:
            row = con.execute(f"SELECT COUNT(*) AS n FROM {tbl}").fetchone()
        except sqlite3.OperationalError:
            rows.append([tbl, "—"])
            continue
        rows.append([tbl, fmt_int(int(row["n"]))])
    console.print(simple_table(["Table", "Rows"], rows))


def _report_blob_sizes(console: Console, con: sqlite3.Connection) -> None:
    section(console, "F2: blob-size distribution")
    try:
        rows = con.execute("SELECT LENGTH(values_blob) AS n FROM Traces").fetchall()
    except sqlite3.OperationalError:
        console.print("  skipped (column Traces.values_blob missing)")
    else:
        sizes = [int(r["n"]) for r in rows]
        if sizes:
            total = sum(sizes)
            console.print(f"  Traces.values_blob: {len(sizes):,} rows, total {_sizes(total)}")
            console.print(
                simple_table(
                    ["Size bucket", "Rows", "Bytes", "Share"],
                    [
                        [label, fmt_int(count), _sizes(byts), pct(byts, total)]
                        for label, count, byts in _histogram(sizes)
                    ],
                )
            )

    try:
        rows = con.execute("SELECT LENGTH(keys_blob) AS n FROM DepKeySets").fetchall()
    except sqlite3.OperationalError:
        return
    sizes = [int(r["n"]) for r in rows]
    if sizes:
        console.print(f"  DepKeySets.keys_blob: {len(sizes):,} rows, total {_sizes(sum(sizes))}")


def _report_depkeyset_sharing(console: Console, con: sqlite3.Connection) -> None:
    section(console, "F3: DepKeySet sharing distribution")
    try:
        rows = con.execute(
            "SELECT traces_per_dks, COUNT(*) AS n FROM "
            "(SELECT dep_key_set_id, COUNT(*) AS traces_per_dks FROM Traces "
            " GROUP BY dep_key_set_id) "
            "GROUP BY traces_per_dks ORDER BY traces_per_dks"
        ).fetchall()
    except sqlite3.OperationalError:
        console.print("  skipped (schema mismatch)")
        return
    if not rows:
        return
    console.print(
        simple_table(
            ["traces per DepKeySet", "DepKeySets with that count"],
            [[fmt_int(int(r["traces_per_dks"])), fmt_int(int(r["n"]))] for r in rows],
        )
    )


def _report_history(
    console: Console,
    con: sqlite3.Connection,
    vocab: sqlite3.Connection | None,
    top_n: int,
) -> None:
    section(console, "F4: History distribution per attr_path")
    try:
        pair_row = con.execute(
            "SELECT COUNT(*) AS n FROM (SELECT DISTINCT recovery_key, attr_path_id FROM History)"
        ).fetchone()
        rows = con.execute(
            "SELECT attr_path_id, COUNT(*) AS n FROM History "
            "GROUP BY attr_path_id ORDER BY n DESC LIMIT ?",
            (top_n,),
        ).fetchall()
    except sqlite3.OperationalError:
        console.print("  skipped (schema mismatch)")
        return
    pair_count = int(pair_row["n"]) if pair_row else 0
    console.print(f"  distinct (recovery_key, attr_path_id) pairs: {fmt_int(pair_count)}")

    if vocab is not None:
        headers = ["attr_path_id", "attr path", "traces"]
        table_rows = [
            [
                str(int(r["attr_path_id"])),
                _resolve_attr_path(vocab, int(r["attr_path_id"])),
                fmt_int(int(r["n"])),
            ]
            for r in rows
        ]
    else:
        headers = ["attr_path_id", "traces"]
        table_rows = [[str(int(r["attr_path_id"])), fmt_int(int(r["n"]))] for r in rows]
    console.print(simple_table(headers, table_rows))


def _report_trace_dks_order(console: Console, con: sqlite3.Connection) -> None:
    section(console, "F6: trace_id vs dep_key_set_id correlation (top 20 newest)")
    try:
        rows = con.execute(
            "SELECT id, dep_key_set_id FROM Traces ORDER BY id DESC LIMIT 20"
        ).fetchall()
    except sqlite3.OperationalError:
        return
    if not rows:
        return
    console.print(
        simple_table(
            ["trace_id", "dep_key_set_id"],
            [[fmt_int(int(r["id"])), fmt_int(int(r["dep_key_set_id"]))] for r in rows],
        )
    )


def _report_multi_attr_sharing(console: Console, con: sqlite3.Connection) -> None:
    section(console, "F7: multi-attr-path trace sharing")
    try:
        row = con.execute(
            "SELECT COUNT(*) AS n FROM "
            "(SELECT trace_id, COUNT(DISTINCT attr_path_id) AS k FROM History "
            " GROUP BY trace_id HAVING k > 1)"
        ).fetchone()
    except sqlite3.OperationalError:
        return
    n = int(row["n"]) if row else 0
    console.print(f"  trace_ids appearing in >1 attr_path in History: {fmt_int(n)}")


def _report_pragma(console: Console, con: sqlite3.Connection) -> None:
    section(console, "F8: SQLite pragmas")
    # Pragma output uses `box=None` (flat formatting) which `simple_table`
    # doesn't configure — build the table directly so pragmas still look like a list.
    t = Table(show_header=False, box=None)
    t.add_column("Pragma", style="bold")
    t.add_column("Value")
    for pragma in ("page_count", "page_size", "freelist_count"):
        try:
            val = con.execute(f"PRAGMA {pragma}").fetchone()[0]
        except sqlite3.OperationalError:
            val = "—"
        t.add_row(pragma, str(val))
    console.print(t)


def _table_counts(con: sqlite3.Connection) -> dict[str, int | None]:
    out: dict[str, int | None] = {}
    for tbl in TRACE_TABLES:
        try:
            row = con.execute(f"SELECT COUNT(*) AS n FROM {tbl}").fetchone()
        except sqlite3.OperationalError:
            out[tbl] = None
            continue
        out[tbl] = int(row["n"])
    return out


def _blob_totals(con: sqlite3.Connection) -> tuple[int, int, int]:
    """Return (n_traces, bytes_values_blob, bytes_keys_blob)."""
    try:
        vrows = con.execute("SELECT LENGTH(values_blob) AS n FROM Traces").fetchall()
    except sqlite3.OperationalError:
        vrows = []
    try:
        krows = con.execute("SELECT LENGTH(keys_blob) AS n FROM DepKeySets").fetchall()
    except sqlite3.OperationalError:
        krows = []
    return len(vrows), sum(int(r["n"]) for r in vrows), sum(int(r["n"]) for r in krows)


def _multi_attr_sharing(con: sqlite3.Connection) -> int:
    try:
        row = con.execute(
            "SELECT COUNT(*) AS n FROM "
            "(SELECT trace_id, COUNT(DISTINCT attr_path_id) AS k FROM History "
            " GROUP BY trace_id HAVING k > 1)"
        ).fetchone()
    except sqlite3.OperationalError:
        return 0
    return int(row["n"]) if row else 0


def _report_compare(
    console: Console,
    a_con: sqlite3.Connection,
    b_con: sqlite3.Connection,
    a_label: str,
    b_label: str,
) -> None:
    """G4 — side-by-side DB-state comparison."""
    section(console, f"G4: DB-state comparison ({a_label} vs {b_label})")

    # F1 side-by-side.
    a_counts = _table_counts(a_con)
    b_counts = _table_counts(b_con)
    rows: list[list[str]] = []
    for tbl in TRACE_TABLES:
        ac = a_counts[tbl]
        bc = b_counts[tbl]
        ratio = f"{bc / ac:.2f}x" if ac and bc is not None else "—"
        rows.append(
            [
                tbl,
                fmt_int(ac) if ac is not None else "—",
                fmt_int(bc) if bc is not None else "—",
                ratio,
            ]
        )
    console.print(
        simple_table(["Table", a_label, b_label, f"{b_label}/{a_label}"], rows, title="row counts")
    )

    # Blob totals.
    a_n, a_v, a_k = _blob_totals(a_con)
    b_n, b_v, b_k = _blob_totals(b_con)
    console.print(
        simple_table(
            ["Metric", a_label, b_label, f"{b_label}/{a_label}"],
            [
                [
                    "Traces rows",
                    fmt_int(a_n),
                    fmt_int(b_n),
                    f"{b_n / a_n:.2f}x" if a_n else "—",
                ],
                [
                    "values_blob bytes",
                    _sizes(a_v),
                    _sizes(b_v),
                    f"{b_v / a_v:.2f}x" if a_v else "—",
                ],
                [
                    "keys_blob bytes",
                    _sizes(a_k),
                    _sizes(b_k),
                    f"{b_k / a_k:.2f}x" if a_k else "—",
                ],
            ],
            title="blob totals",
        )
    )

    # Multi-attr trace sharing.
    a_multi = _multi_attr_sharing(a_con)
    b_multi = _multi_attr_sharing(b_con)
    console.print(
        simple_table(
            ["Metric", a_label, b_label, "Δ"],
            [
                [
                    "trace_ids in >1 attr_path",
                    fmt_int(a_multi),
                    fmt_int(b_multi),
                    fmt_int(b_multi - a_multi),
                ],
            ],
        )
    )


def _find_trace_db(path: Path) -> Path:
    if path.is_file():
        return path
    if not path.is_dir():
        raise SystemExit(f"{path} is neither a file nor a directory")
    candidates = sorted(
        path.glob("eval-trace-*.sqlite"), key=lambda p: p.stat().st_mtime, reverse=True
    )
    if not candidates:
        raise SystemExit(f"no eval-trace-*.sqlite in {path}")
    return candidates[0]


def _find_vocab_db(base: Path) -> Path | None:
    candidate = (base.parent if base.is_file() else base) / "attr-vocab.sqlite"
    return candidate if candidate.is_file() else None


def _run_single_db(
    console: Console,
    trace_db: Path,
    vocab_db_path: Path | None,
    top_n: int,
) -> None:
    console.print(f"[bold]DB:[/bold] {trace_db}")
    if vocab_db_path:
        console.print(f"[bold]Vocab:[/bold] {vocab_db_path}")
    console.print()
    with _open(trace_db) as con:
        vocab_con = _open(vocab_db_path) if vocab_db_path else None
        try:
            _report_tables(console, con)
            _report_blob_sizes(console, con)
            _report_depkeyset_sharing(console, con)
            _report_history(console, con, vocab_con, top_n)
            _report_trace_dks_order(console, con)
            _report_multi_attr_sharing(console, con)
            _report_pragma(console, con)
        finally:
            if vocab_con is not None:
                vocab_con.close()


def register(app: App) -> None:
    @app.command(name="db-inspect")  # pyright: ignore[reportUnusedFunction]
    def db_inspect(
        db: Path = DEFAULT_NIX_CACHE,
        db2: Path | None = None,
        vocab: Path | None = None,
        top_n: int = 20,
        output: Path | None = None,
    ) -> int:
        """Post-mortem inspection of an eval-trace SQLite DB (F1-F8).

        When `--db2` is supplied, also emit a side-by-side comparison
        of row counts, blob totals, and multi-attr trace sharing
        between the two DBs (plan G4) — useful for checking whether a
        code change produced a different DB shape, not just different
        wall times.

        Parameters
        ----------
        db: Path to an eval-trace-*.sqlite OR a cache dir holding one.
        db2: Second DB to compare against (enables G4 compare view).
        vocab: attr-vocab.sqlite (default: sibling of --db).
        top_n: Top-N attr paths to surface in F4.
        output: Save report (.html for rich markup, else plain text).
        """
        trace_db = _find_trace_db(db)
        vocab_db_path = vocab or _find_vocab_db(db)
        console = Console(record=True)
        _run_single_db(console, trace_db, vocab_db_path, top_n)

        if db2 is not None:
            trace_db2 = _find_trace_db(db2)
            section(console, f"— second DB: {trace_db2}")
            _run_single_db(console, trace_db2, _find_vocab_db(db2), top_n)
            a_label = trace_db.parent.name or trace_db.stem
            b_label = trace_db2.parent.name or trace_db2.stem
            with _open(trace_db) as a_con, _open(trace_db2) as b_con:
                _report_compare(console, a_con, b_con, a_label, b_label)

        if output:
            export_console(console, output)
        return 0
