# A floating content-addressed derivation whose output differs on every build,
# unless it is deterministic.
{
  busybox,
  tag,
  deterministic ? false,
}:
derivation {
  name = "ca-nondet-${tag}";
  system = builtins.currentSystem;
  builder = busybox;
  args = [
    "sh"
    "-c"
    (
      if deterministic then
        "echo ${tag} > $out"
      else
        "read -r x < /proc/sys/kernel/random/uuid; echo $x > $out"
    )
  ];
  __contentAddressed = true;
  outputHashMode = "recursive";
  outputHashAlgo = "sha256";
}
