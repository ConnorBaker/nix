"""Rich-based render helpers.

Declarative helpers for the three idioms that dominate every subcommand:

- `simple_table(headers, rows)`: raw table with right-justified cells.
- `metric_table(...)`: rows of metrics × series with optional
  ratio-vs-reference columns — used by `runs`, `logs`, `pairwise`.
- `per_label_report(...)`: "build one subtable per label, but collapse
  if every label's subtable would be identical" — the idiom that
  `logs` uses for daemon-ops, passes, deps, etc.
"""

from __future__ import annotations

from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from rich.console import Console
from rich.panel import Panel
from rich.table import Table
from rich.text import Text


def section(console: Console, title: str) -> None:
    console.print(Panel(f"[bold]{title}[/bold]", expand=False))


def subsection(console: Console, title: str) -> None:
    console.print(f"\n[bold dim]{title}[/bold dim]")


def pct(num: float, den: float) -> str:
    return f"{100 * num / den:.1f}%" if den else "N/A"


def ratio_str(val: float, ref: float) -> str:
    return f"{val / ref:.2f}x" if ref else "N/A"


def fmt_int(n: int | float) -> str:
    return f"{n:,.0f}" if isinstance(n, float) and not n.is_integer() else f"{int(n):,}"


def fmt_us(us: float) -> str:
    if us < 1_000:
        return f"{us:.0f}us"
    if us < 1_000_000:
        return f"{us / 1000:.1f}ms"
    return f"{us / 1_000_000:.2f}s"


def pass_fail(ok: bool) -> Text:
    return Text("PASS", style="bold green") if ok else Text("FAIL", style="bold red")


def short_commit(commit: str) -> str:
    return commit[:12]


def delta_text(val: float, unit: str = "s") -> Text:
    style = "red" if val > 0 else "green" if val < 0 else ""
    return Text(f"{val:+.2f}{unit}", style=style)


def export_console(console: Console, output: Path) -> None:
    if output.suffix == ".html":
        output.write_text(console.export_html())
    else:
        output.write_text(console.export_text())


# -- Table helpers ----------------------------------------------------------


def simple_table(
    headers: Sequence[str],
    rows: Iterable[Sequence[str | Text | Any]],
    *,
    title: str | None = None,
) -> Table:
    """Small table with right-justified cells after the first column."""
    t = Table(title=title, show_header=True, header_style="bold")
    for i, h in enumerate(headers):
        t.add_column(h, justify="right" if i > 0 else "left")
    for row in rows:
        t.add_row(*(c if isinstance(c, Text) else str(c) for c in row))
    return t


@dataclass(frozen=True)
class MetricRow:
    """One row in a `metric_table`: a label plus per-series values."""

    label: str
    values: dict[str, float | None]


def metric_table(
    rows: Sequence[MetricRow],
    series: Sequence[str],
    *,
    title: str | None = None,
    fmt: Callable[[float], str] | None = None,
    row_fmt: Callable[[float, str], str] | None = None,
    ratios_against: str | None = None,
) -> Table:
    """Render `label × series` grid with optional ratio-vs-reference columns.

    Fills missing cells with "—".  When `ratios_against` is set, each
    non-reference series gets a `{name}/{ref}` column right after the
    value columns.

    Pass `row_fmt(value, row_label)` when formatting depends on the
    metric identity (e.g. "display ms for `*TimeUs` rows, integers
    otherwise").  Otherwise use `fmt(value)` for a uniform style.
    """

    def _default_fmt(v: float) -> str:
        return f"{v:.0f}"

    simple = fmt or _default_fmt
    cell_fmt: Callable[[float, str], str] = row_fmt or (lambda v, _: simple(v))

    t = Table(title=title, show_header=True, header_style="bold")
    t.add_column("Metric")
    for s in series:
        t.add_column(s, justify="right")
    if ratios_against is not None:
        for s in series:
            if s != ratios_against:
                t.add_column(f"{s}/{ratios_against}", justify="right")

    for row in rows:
        cells: list[str] = [row.label]
        values = row.values
        for s in series:
            v = values.get(s)
            cells.append(cell_fmt(v, row.label) if v is not None else "—")
        if ratios_against is not None:
            ref_val = values.get(ratios_against)
            for s in series:
                if s == ratios_against:
                    continue
                val = values.get(s)
                if val is not None and ref_val is not None:
                    cells.append(ratio_str(val, ref_val))
                else:
                    cells.append("—")
        t.add_row(*cells)
    return t


def per_label_report(
    console: Console,
    labels: Sequence[str],
    renderer: Callable[[str], object | None],
    *,
    equality_key: Callable[[str], object] | None = None,
    identical_note: str = "Identical across: {labels}",
) -> None:
    """Render one sub-table per label, collapsing identical outputs.

    `renderer(label)` returns the thing to print for `label` (a `Table`,
    `str`, or `None` to skip).  When `equality_key` is provided, labels
    that compare equal have their output printed only once along with
    an `identical_note` naming the shared labels.
    """
    if equality_key is None:
        for lbl in labels:
            out = renderer(lbl)
            if out is not None:
                console.print(out)
        return

    # Group labels by equality_key; labels with the same key render once.
    groups: list[tuple[list[str], object]] = []
    for lbl in labels:
        key = equality_key(lbl)
        for group_labels, group_key in groups:
            if group_key == key:
                group_labels.append(lbl)
                break
        else:
            groups.append(([lbl], key))

    for group_labels, _ in groups:
        if len(group_labels) > 1:
            console.print(f"\n[dim]{identical_note.format(labels=', '.join(group_labels))}[/dim]")
        out = renderer(group_labels[0])
        if out is not None:
            console.print(out)
