# a's drvPath fails (no `system`), but only once forced, after the search;
# the search fails first, in b.
{
  a = derivation {
    name = "a";
    builder = "/bin/sh";
  };
  b = throw "walk error in b";
}
