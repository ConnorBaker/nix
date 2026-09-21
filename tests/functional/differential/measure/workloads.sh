#!/usr/bin/env bash
# Generate the measurement workloads into $1.  Two shapes that bracket the
# real cases, plus a toFile-heavy one:
#   closure.nix   one top-level derivation over a wide .drv closure (the
#                 `nix-instantiate -A pkg` shape: one flush writes the closure)
#   fan.nix       N independent top-level derivations, each printed (the
#                 `nix-instantiate big-list.nix` shape: one flush per print)
#   files.nix     one derivation whose inputs are N `builtins.toFile` texts
set -eu
dir=$1
n=${2:-2000}
mkdir -p "$dir"

cat > "$dir/closure.nix" <<EOF
let
  leaf = i: derivation {
    name = "measure-leaf-\${toString i}";
    system = "x86_64-linux";
    builder = "/bin/sh";
    args = [ "-c" "echo \${toString i} > \$out" ];
  };
  leaves = builtins.genList leaf $n;
in derivation {
  name = "measure-root";
  system = "x86_64-linux";
  builder = "/bin/sh";
  args = [ "-c" "cat \${builtins.concatStringsSep " " (map (d: d.outPath) leaves)} > \$out" ];
}
EOF

cat > "$dir/fan.nix" <<EOF
builtins.genList (i: derivation {
  name = "measure-fan-\${toString i}";
  system = "x86_64-linux";
  builder = "/bin/sh";
  args = [ "-c" "echo \${toString i} > \$out" ];
}) $n
EOF

cat > "$dir/files.nix" <<EOF
let
  files = builtins.genList (i: builtins.toFile "measure-file-\${toString i}" "content \${toString i}") $n;
in derivation {
  name = "measure-files";
  system = "x86_64-linux";
  builder = "/bin/sh";
  args = [ "-c" "cat \${builtins.concatStringsSep " " files} > \$out" ];
}
EOF
echo "workloads in $dir (n=$n)"
