"""Top-level CLI.

Uses cyclopts for declarative subcommand registration.  Each
subcommand module defines a `register(app)` function that attaches
its handlers to the top-level `App`.
"""

from __future__ import annotations

from cyclopts import App

from .subcommands import (
    classify_cmd,
    db_inspect_cmd,
    export_cmd,
    generate_cmd,
    logs_cmd,
    pairwise_cmd,
    runs_cmd,
    series_cmd,
    simulate_cmd,
    sv_cmd,
)


def _build_app() -> App:
    app = App(
        name="eval-trace-bench",
        help="Run, compare, and analyse eval-trace benchmarks.",
    )
    for mod in (
        generate_cmd,
        runs_cmd,
        logs_cmd,
        classify_cmd,
        series_cmd,
        pairwise_cmd,
        db_inspect_cmd,
        sv_cmd,
        simulate_cmd,
        export_cmd,
    ):
        mod.register(app)
    return app


def main(argv: list[str] | None = None) -> int:
    return int(_build_app()(argv) or 0)


if __name__ == "__main__":
    raise SystemExit(main())
