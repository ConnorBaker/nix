#!/usr/bin/env python3
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import time

BASE = Path(__file__).resolve().parent
REV = "6aefcda9401be8acc2b74244fb3b37520ea1f0a8"
BINS = {}
URIS = {}
SYSTEM = "aarch64-linux"
ATTRS = [
    "hello", "jq", "git", "openssl", "curl", "zlib", "bash", "coreutils",
    "python3", "nodejs", "rustc", "cargo", "gcc", "clang", "cmake", "ninja",
    "pkg-config", "gnumake", "ripgrep", "fd", "sqlite", "postgresql", "valkey", "nginx",
    "openssh", "systemd", "neovim", "emacs", "ffmpeg", "imagemagick", "gimp", "firefox",
]
CPUS = ",".join(map(str, sorted(os.sched_getaffinity(0))))
CONFIG = """experimental-features = nix-command flakes git-hashing
sandbox = false
substituters =
build-users-group =
eval-cache = false
flake-registry =
accept-flake-config = false
allow-import-from-derivation = false
gc-reserved-space = 0
"""


def expression(scheme, count):
    names = " ".join(json.dumps(x) for x in ATTRS[:count])
    return f"""let
      flake = builtins.getFlake {json.dumps(URIS[scheme])};
      pkgs = flake.legacyPackages.{SYSTEM};
    in builtins.map (attr: let p = builtins.getAttr attr pkgs; in {{
      inherit attr;
      name = p.name;
      version = p.version or "";
      drvPath = p.drvPath;
    }}) [ {names} ]"""


def environment(state):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("NIX_", "_NIX_", "XDG_", "GC_"))
           and k not in ("LD_LIBRARY_PATH", "LD_PRELOAD")}
    for d in ["etc", "cache", "state", "data", "root", "tmp"]:
        (state / d).mkdir(parents=True, exist_ok=True)
    env.update({
        "NIX_CONF_DIR": str(state / "etc"),
        "NIX_USER_CONF_FILES": "/dev/null",
        "NIX_CONFIG": CONFIG,
        "XDG_CACHE_HOME": str(state / "cache"),
        "XDG_STATE_HOME": str(state / "state"),
        "XDG_DATA_HOME": str(state / "data"),
        "TMPDIR": str(state / "tmp"),
        "LC_ALL": "C.UTF-8",
    })
    return env


def run(label, scheme, count, phase, trial):
    identity = f"{phase}-{count}-{scheme}-{label}-{trial:02d}"
    state_name = identity if phase in ("cold", "pilot") else f"warm-{scheme}-{label}"
    state = BASE / "states" / state_name
    if phase in ("cold", "pilot") and state.exists():
        raise RuntimeError(f"Refusing to reuse first-use state: {state}")
    env = environment(state)
    logs = BASE / "logs"
    logs.mkdir(exist_ok=True)
    expr = expression(scheme, count)
    (logs / f"{identity}.nix").write_text(expr + "\n")
    nix_command = [BINS[label], "--store", f"local?root={state / 'root'}",
                   "eval", "--impure", "--json", "--expr", expr]
    command = ["/usr/bin/time", "-f", "%U %S %M %I %O %w %c", "-o",
               str(logs / f"{identity}.time"), "taskset", "-c", CPUS, *nix_command]
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    load = os.getloadavg()
    t0 = time.perf_counter()
    with (logs / f"{identity}.json").open("wb") as out, (logs / f"{identity}.stderr").open("wb") as err:
        proc = subprocess.run(command, env=env, stdout=out, stderr=err)
    elapsed = time.perf_counter() - t0
    result = dict(id=identity, label=label, scheme=scheme, attributes=count, phase=phase,
                  trial=trial, started=started, load_before=load, load_after=os.getloadavg(),
                  wall_s=elapsed, returncode=proc.returncode, command=nix_command)
    timing = (logs / f"{identity}.time").read_text().splitlines()[-1].split()
    for key, val in zip(["user_s", "system_s", "rss_kib", "fs_inputs", "fs_outputs",
                         "voluntary_switches", "involuntary_switches"], timing):
        result[key] = float(val)
    if proc.returncode == 0:
        output = json.loads((logs / f"{identity}.json").read_text())
        assert len(output) == count
        assert [x["attr"] for x in output] == ATTRS[:count]
        assert all(x["drvPath"].startswith("/nix/store/") and x["drvPath"].endswith(".drv") for x in output)
        normalized = [{k: v for k, v in x.items() if k != "drvPath"} for x in output]
        result["semantic_sha256"] = hashlib.sha256(json.dumps(normalized, sort_keys=True).encode()).hexdigest()
        result["output_sha256"] = hashlib.sha256(json.dumps(output, sort_keys=True).encode()).hexdigest()
        paths = state / "root/nix/store"
        result["derivation_files"] = sum(1 for _ in paths.glob("*.drv"))
    with (BASE / "samples.jsonl").open("a") as f:
        f.write(json.dumps(result) + "\n")
    print(f"{identity}: rc={proc.returncode} wall={elapsed:.3f}s cpu={result['user_s'] + result['system_s']:.3f}s rss={result['rss_kib']/1024:.1f}MiB", flush=True)
    if proc.returncode:
        raise RuntimeError((logs / f"{identity}.stderr").read_text()[-6000:])
    return result


def metadata():
    obj = dict(branch_revision=args.branch_revision,
               base_revision=args.base_revision,
               nixpkgs_revision=REV, system=SYSTEM, bins=BINS, uris=URIS,
               attributes=ATTRS, cpu_affinity=CPUS, config=CONFIG,
               started=datetime.datetime.now(datetime.timezone.utc).isoformat(),
               kernel=os.uname().release,
               versions={k: subprocess.check_output([v, "--version"], text=True).strip() for k, v in BINS.items()})
    (BASE / "metadata.json").write_text(json.dumps(obj, indent=2) + "\n")


def summarize():
    rows = [json.loads(x) for x in (BASE / "samples.jsonl").read_text().splitlines()]
    rows = [r for r in rows if r["phase"] in ("cold", "warm")]
    summary = []
    for phase, count in [("cold", 32), ("warm", 8), ("warm", 32)]:
        for scheme in URIS:
            entry = dict(phase=phase, attributes=count, scheme=scheme)
            both = [r for r in rows if (r["phase"], r["attributes"], r["scheme"]) == (phase, count, scheme)]
            if not both:
                continue
            assert len({r["semantic_sha256"] for r in both}) == 1
            for label in BINS:
                part = [r for r in both if r["label"] == label]
                vals = [r["wall_s"] for r in part]
                if not vals:
                    continue
                med = statistics.median(vals)
                entry[label] = dict(n=len(part), wall_median_s=med,
                                    wall_mad_s=statistics.median(abs(x-med) for x in vals),
                                    wall_min_s=min(vals), wall_max_s=max(vals),
                                    cpu_median_s=statistics.median(r["user_s"]+r["system_s"] for r in part),
                                    rss_median_mib=statistics.median(r["rss_kib"]/1024 for r in part))
            if all(label in entry for label in BINS):
                entry["branch_over_base"] = entry["branch"]["wall_median_s"] / entry["base"]["wall_median_s"]
            summary.append(entry)
    (BASE / "summary.json").write_text(json.dumps(summary, indent=2)+"\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["pilot", "benchmark", "summarize"])
    parser.add_argument("--base-nix", required=True)
    parser.add_argument("--branch-nix", required=True)
    parser.add_argument("--base-revision", default="8aad447f0bace6be413963e897ad690247ad6aa2")
    parser.add_argument("--branch-revision", required=True)
    parser.add_argument("--path-source", type=Path, required=True)
    parser.add_argument("--git-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", default=REV)
    parser.add_argument("--system", default=SYSTEM)
    parser.add_argument("--cpus", default=CPUS)
    parser.add_argument("--cold-trials", type=int, default=3)
    parser.add_argument("--warm-trials", type=int, default=10)
    args = parser.parse_args()
    BASE = args.output.resolve()
    BASE.mkdir(parents=True, exist_ok=True)
    REV, SYSTEM, CPUS = args.revision, args.system, args.cpus
    BINS = {"base": args.base_nix, "branch": args.branch_nix}
    URIS = {
        "path": f"path:{args.path_source.resolve()}",
        "git": f"git+file://{args.git_source.resolve()}?shallow=1",
        "github": f"github:NixOS/nixpkgs/{REV}",
    }
    if args.mode == "pilot":
        metadata()
        for scheme in URIS:
            for label in BINS:
                run(label, scheme, 32, "pilot", 0)
    elif args.mode == "benchmark":
        metadata()
        for trial in range(args.cold_trials):
            for i, scheme in enumerate(URIS):
                labels = list(BINS) if (trial+i)%2 == 0 else list(reversed(BINS))
                for label in labels:
                    run(label, scheme, 32, "cold", trial)
        for scheme in URIS:
            for label in BINS:
                run(label, scheme, 32, "seed", 0)
        rng = random.Random(20260921)
        for trial in range(args.warm_trials):
            jobs = [(s, n) for s in URIS for n in [8, 32]]
            rng.shuffle(jobs)
            for i, (scheme, count) in enumerate(jobs):
                labels = list(BINS) if (trial+i)%2 == 0 else list(reversed(BINS))
                for label in labels:
                    run(label, scheme, count, "warm", trial)
        summarize()
    else:
        summarize()
