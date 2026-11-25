with import ./config.nix;

{

  infiniteLoop = mkDerivation {
    name = "timeout";
    buildCommand = ''
      touch $out
      echo "'timeout' builder entering an infinite loop"
      while true ; do echo -n .; done
    '';
  };

  silent = mkDerivation {
    name = "silent";
    buildCommand = ''
      touch $out
      sleep 60
    '';
  };

  # Closes its log and exits a moment later, as GNU coreutils programs do.
  closeLogThenExit = mkDerivation {
    name = "close-log-then-exit";
    buildCommand = ''
      touch $out
      exec > /dev/null 2>&1
      sleep 1
    '';
  };

  # Exits with 3, leaving a process that writes to the log after it has
  # exited, and one that doesn't hold the log.
  leavesProcesses = mkDerivation {
    name = "leaves-processes";
    marker = builtins.getEnv "TEST_ROOT" + "/marker";
    buildCommand = ''
      (sleep 1; echo late) &
      (sleep 5; touch "$marker") < /dev/null > /dev/null 2>&1 &
      exit 3
    '';
  };

  closeLog = mkDerivation {
    name = "silent";
    buildCommand = ''
      touch $out
      exec > /dev/null 2>&1
      sleep 1000000000
    '';
  };

}
