# A derivation in a file of its own: every import of it is one set.
derivation {
  name = "imported";
  system = "x86_64-linux";
  builder = "/bin/sh";
}
