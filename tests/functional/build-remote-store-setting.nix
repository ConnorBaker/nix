{ busybox, name }:

derivation {
  name = "build-remote-store-setting-${name}";
  system = builtins.currentSystem;
  builder = busybox;
  args = [
    "sh"
    "-c"
    "echo ${name} > $out"
  ];
}
