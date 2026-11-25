with import ./config.nix;

{

  # Closes its log, keeps working for a while, then exits successfully.
  closesLogThenSucceeds = mkDerivation {
    name = "closes-log-then-succeeds";
    buildCommand = ''
      exec > /dev/null 2>&1
      sleep 2
      touch $out
    '';
  };

  # The same, but fails with its own exit code.
  closesLogThenFails = mkDerivation {
    name = "closes-log-then-fails";
    buildCommand = ''
      exec > /dev/null 2>&1
      sleep 2
      exit 3
    '';
  };

  # Leaves a process behind that doesn't hold the log.
  leavesChild = mkDerivation {
    name = "leaves-child";
    pidFile = builtins.getEnv "TEST_ROOT" + "/leftover.pid";
    buildCommand = ''
      sleep 1000000 > /dev/null 2>&1 < /dev/null &
      echo $! > "$pidFile"
      touch $out
    '';
  };

  # Exits while a process it started still holds the log and writes to
  # it afterwards.
  exitsBeforeLogCloses = mkDerivation {
    name = "exits-before-log-closes";
    buildCommand = ''
      touch $out
      (sleep 2; echo "written after the builder exited") &
    '';
  };

  # Writes a lot of log quickly, then fails.
  fastLogThenFails = mkDerivation {
    name = "fast-log-then-fails";
    buildCommand = ''
      for ((i = 0; i < 10000; i++)); do
        echo "line $i"
      done
      echo "last line"
      exit 1
    '';
  };

}
