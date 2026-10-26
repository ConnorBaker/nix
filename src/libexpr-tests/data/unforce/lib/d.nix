# A derivation: equal arguments give equal drvPaths, but each call builds
# a set of its own.
name:
derivation {
  inherit name;
  system = "x86_64-linux";
  builder = "/bin/sh";
}
