#!/usr/bin/env python3
"""Profile the exact commands and warm stores of a nixpkgs-evaluation.py run.

Profiling is separate from timing: neither perf nor strace times are benchmark
samples. Cold profiles each use a fresh store and fetcher cache.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("benchmark", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--tool", choices=["perf", "strace"], required=True)
parser.add_argument("--event", default="cycles:u")
parser.add_argument("--attributes", type=int, choices=[8, 32], default=32)
parser.add_argument("--repeat", type=int, default=1)
parser.add_argument("--schemes", nargs="+", default=["path", "git", "github"])
parser.add_argument("--phases", nargs="+", choices=["cold", "warm"], default=["cold", "warm"])
args = parser.parse_args()
args.benchmark = args.benchmark.resolve()
args.output = args.output.resolve()
metadata = json.loads((args.benchmark / "metadata.json").read_text())
spec = importlib.util.spec_from_file_location("benchmark", Path(__file__).with_name("nixpkgs-evaluation.py"))
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)
b.URIS = metadata["uris"]
b.SYSTEM = metadata["system"]
b.ATTRS = metadata["attributes"]
b.CONFIG = metadata["config"]
args.output.mkdir(parents=True, exist_ok=True)

for phase in args.phases:
    for scheme in args.schemes:
        for label, binary in metadata["bins"].items():
            name = f"{args.tool}-{phase}-{args.attributes}-{scheme}-{label}"
            prefix = args.output / name
            if prefix.with_suffix(".json").exists():
                raise RuntimeError(f"Refusing to replace profile {name}")
            state = (args.benchmark / "states" / f"warm-{scheme}-{label}" if phase == "warm"
                     else args.output / "states" / name)
            if phase == "cold" and state.exists():
                raise RuntimeError(f"Refusing to reuse first-use state {state}")
            if phase == "warm" and not state.exists():
                raise RuntimeError(f"Missing populated state {state}")
            command = [binary, "--store", f"local?root={state / 'root'}", "eval", "--impure",
                       "--json", "--expr", b.expression(scheme, args.attributes)]
            if args.repeat != 1:
                if phase != "warm" or args.repeat < 1:
                    raise RuntimeError("Only warm profiles can repeat")
                command = ["bash", "-c", 'for ((i=0; i<$1; ++i)); do "${@:2}" || exit; done',
                           "profile", str(args.repeat), *command]
            command = ["taskset", "-c", metadata["cpu_affinity"], *command]
            if args.tool == "perf":
                command = ["perf", "--buildid-dir", str(args.output / "buildids"), "record",
                           "-e", args.event, "-c", "10000000", "-m", "4M", "--call-graph",
                           "dwarf,16384", "-o", str(prefix) + ".data", "--", *command]
            else:
                command = ["strace", "-f", "-c", "-o", str(prefix) + ".syscalls", *command]
            print("START", name, flush=True)
            with open(str(prefix) + ".stdout", "wb") as out, open(str(prefix) + ".stderr", "wb") as err:
                result = subprocess.run(command, env=b.environment(state), stdout=out, stderr=err)
            Path(str(prefix) + ".json").write_text(json.dumps(dict(
                command=command, returncode=result.returncode,
                branch_revision=metadata["branch_revision"], base_revision=metadata["base_revision"]), indent=2) + "\n")
            result.check_returncode()
            print("END", name, flush=True)
