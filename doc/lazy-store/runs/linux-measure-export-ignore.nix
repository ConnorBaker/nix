/*
  The export-ignore filter's cost for a commit fetched from a checkout,
  before and after the filter's attributes became the named tree's:
  `doc/lazy-store/01-specification.md` section 10, *The `export-ignore`
  filter's attributes are the named tree's*; the two tables in
  `05-validation.md` section 6, *The export-ignore filter's lookups* and
  *The attributes a commit's filter consults*.  The driver beside it is
  `linux-measure-export-ignore.sh`.

  A tree is copied, committed and repacked inside the sandbox, so the
  repository has a working directory; `hello.name` is then evaluated through
  `builtins.fetchGit` four ways, with a fresh store, state directory and
  cache each time, and `strace -f -c -w` counts one warm run of each.

  nix build --no-link --print-out-paths -L --system aarch64-linux \
    -f linux-measure-export-ignore.nix --argstr beforeNix <store path> \
    --argstr afterNix <store path> --argstr nixpkgsSrc <store path>
*/
{
  nixpkgsSrc,
  beforeNix,
  afterNix,
  system ? "aarch64-linux",
}:
let
  pkgs = import nixpkgsSrc { inherit system; };
in
pkgs.runCommand "measure-export-ignore"
  {
    nativeBuildInputs = [
      pkgs.git
      pkgs.strace
      pkgs.gnugrep
      pkgs.gawk
    ];
    beforeNix = builtins.storePath beforeNix;
    afterNix = builtins.storePath afterNix;
    src = builtins.storePath nixpkgsSrc;
    passAsFile = [ "script" ];
    script = builtins.readFile ./linux-measure-export-ignore.sh;
  }
  ''
    bash $scriptPath 2>&1 | tee $out
  ''
