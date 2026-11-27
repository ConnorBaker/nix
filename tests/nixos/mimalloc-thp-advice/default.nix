# Nix's mimalloc must neither ask for transparent huge pages for its arenas
# (madvise MADV_HUGEPAGE) nor disable them for the process
# (prctl PR_SET_THP_DISABLE, which every process Nix starts would inherit).
# See `mimalloc` in packaging/dependencies.nix.
#
# mimalloc advises only where /sys/kernel/mm/transparent_hugepage/enabled is
# not `[never]`, so the guest's kernel command line sets it to `madvise`,
# whatever the host's setting.
{
  name = "mimalloc-thp-advice";

  nodes.machine =
    { pkgs, ... }:
    {
      boot.kernelParams = [ "transparent_hugepage=madvise" ];
      environment.systemPackages = [ pkgs.strace ];
    };

  testScript = ''
    machine.wait_for_unit("multi-user.target")
    machine.succeed("grep -qF '[madvise]' /sys/kernel/mm/transparent_hugepage/enabled")
    machine.succeed("strace -f -o /tmp/trace -e trace=open,openat,madvise,prctl nix --version >&2")
    machine.succeed("bash ${./check-trace.sh} /tmp/trace >&2")
  '';
}
