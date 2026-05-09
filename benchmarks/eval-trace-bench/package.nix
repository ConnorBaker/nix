{
  lib,
  python3,
  pyright,
  ruff,
}:

python3.pkgs.buildPythonApplication {
  pname = "eval-trace-bench";
  version = "0.1.0";
  pyproject = true;

  src = lib.cleanSource ./.;

  build-system = [ python3.pkgs.hatchling ];

  dependencies = [
    python3.pkgs.rich
    python3.pkgs.pydantic
    python3.pkgs.cyclopts
  ];

  nativeCheckInputs = [
    python3.pkgs.pytest
    pyright
    ruff
  ];

  # `checkPhase` runs focused semantic tests plus ruff + pyright.
  doCheck = true;
  checkPhase = ''
    runHook preCheck

    echo "-- pytest --"
    pytest -q tests

    echo "-- ruff check --"
    ruff check src tests

    echo "-- ruff format --check --"
    ruff format --check src tests

    echo "-- pyright --"
    pyright --pythonpath ${python3}/bin/python3 src/eval_trace_bench

    runHook postCheck
  '';

  meta = {
    description = "Eval-trace benchmark runner, comparator, and analyzer";
    mainProgram = "eval-trace-bench";
  };
}
