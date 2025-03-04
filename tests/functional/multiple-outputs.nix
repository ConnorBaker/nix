with import ./config.nix;

rec {

  # Want to ensure that "out" doesn't get a suffix on it's path.
  nameCheck = mkDerivation {
    name = "multiple-outputs-a";
    outputs = [
      "out"
      "dev"
    ];
    builder = builtins.toFile "builder.sh" ''
      mkdir $first $second
      test -z $all
      echo "first" > $first/file
      echo "second" > $second/file
      ln -s $first $second/link
    '';
    helloString = "Hello, world!";
  };

  a = mkDerivation {
    name = "multiple-outputs-a";
    outputs = [
      "first"
      "second"
    ];
    builder = builtins.toFile "builder.sh" ''
      mkdir $first $second
      test -z $all
      echo "first" > $first/file
      echo "second" > $second/file
      ln -s $first $second/link
    '';
    helloString = "Hello, world!";
  };

  use-a = mkDerivation {
    name = "use-a";
    inherit (a) first second;
    builder = builtins.toFile "builder.sh" ''
      cat $first/file $second/file >$out
    '';
  };

  b = mkDerivation {
    defaultOutput =
      assert a.second.helloString == "Hello, world!";
      a;
    firstOutput =
      assert a.outputName == "first";
      a.first.first;
    secondOutput =
      assert a.second.outputName == "second";
      a.second.first.first.second.second.first.second;
    allOutputs = a.all;
    name = "multiple-outputs-b";
    builder = builtins.toFile "builder.sh" ''
      mkdir $out
      test "$firstOutput $secondOutput" = "$allOutputs"
      test "$defaultOutput" = "$firstOutput"
      test "$(cat $firstOutput/file)" = "first"
      test "$(cat $secondOutput/file)" = "second"
      echo "success" > $out/file
    '';
  };

  c = mkDerivation {
    name = "multiple-outputs-c";
    drv = b.drvPath;
    builder = builtins.toFile "builder.sh" ''
      mkdir $out
      ln -s $drv $out/drv
    '';
  };

  d = mkDerivation {
    name = "multiple-outputs-d";
    drv = builtins.unsafeDiscardOutputDependency b.drvPath;
    builder = builtins.toFile "builder.sh" ''
      mkdir $out
      echo $drv > $out/drv
    '';
  };

  cyclic =
    (mkDerivation {
      name = "cyclic-outputs";
      outputs = [
        "a"
        "b"
        "c"
      ];
      builder = builtins.toFile "builder.sh" ''
        mkdir $a $b $c
        echo $a > $b/foo
        echo $b > $c/bar
        echo $c > $a/baz
      '';
    }).a;

  e = mkDerivation {
    name = "multiple-outputs-e";
    outputs = [
      "a_a"
      "b"
      "c"
    ];
    meta.outputsToInstall = [
      "a_a"
      "b"
    ];
    buildCommand = "mkdir $a_a $b $c";
  };

  nothing-to-install = mkDerivation {
    name = "nothing-to-install";
    meta.outputsToInstall = [ ];
    buildCommand = "mkdir $out";
  };

  independent = mkDerivation {
    name = "multiple-outputs-independent";
    outputs = [
      "first"
      "second"
    ];
    builder = builtins.toFile "builder.sh" ''
      mkdir $first $second
      test -z $all
      echo "first" > $first/file
      echo "second" > $second/file
    '';
  };

  use-independent = mkDerivation {
    name = "use-independent";
    inherit (a) first second;
    builder = builtins.toFile "builder.sh" ''
      cat $first/file $second/file >$out
    '';
  };

  invalid-output-name-1 = mkDerivation {
    name = "invalid-output-name-1";
    outputs = [ "out/" ];
  };

  invalid-output-name-2 = mkDerivation {
    name = "invalid-output-name-2";
    outputs = [
      "x"
      "foo$"
    ];
  };

}
