# This is a helper to callFlake() over an already-resolved flake graph.

graph:

let
  inherit (builtins) attrNames hasAttr isAttrs isPath mapAttrs;

  allNodes = mapAttrs (
    key: node:
    let
      sourceInfo = node.sourceInfo;
      inputSpecs = node.inputSpecs or { };
      resolvedInputs = node.resolvedInputs or { };
      outPath = node.outPath;
    in
    assert isAttrs sourceInfo;
    assert sourceInfo ? outPath;
    assert isPath node.carrierPath;
    assert hasAttr node.sourceInfoKey graph.nodes;
    assert node.parent == null || hasAttr node.parent graph.nodes;
    assert attrNames inputSpecs == attrNames resolvedInputs;
    if node.isFlake then
      let
        importPath = node.flakePath;
        displayImportPath = node.displayFlakePath or importPath;
        importTarget = builtins.appendPath importPath "flake.nix";
        displayImportTarget = builtins.appendPath displayImportPath "flake.nix";
        flake =
          assert attrNames inputSpecs == attrNames resolvedInputs;
          if displayImportTarget == importTarget then
            import importTarget
          else
            builtins.importDisplayPhysical displayImportTarget importTarget;
        inputs = mapAttrs (
          inputName: inputKey:
          assert builtins.hasAttr inputKey allNodes;
          allNodes.${inputKey}.result
        ) resolvedInputs;
        result =
          let
            outputs = flake.outputs (inputs // { self = result; });
          in
          outputs
          // sourceInfo
          // {
            inherit outPath;
            inherit inputs;
            inherit outputs;
            inherit sourceInfo;
            _type = "flake";
          };
      in
      {
        result =
          assert builtins.isFunction flake.outputs;
          result;

        inherit outPath sourceInfo;
      }
    else
      {
        result = sourceInfo // { inherit sourceInfo outPath; };
        inherit outPath sourceInfo;
      }
  ) graph.nodes;

in
assert hasAttr graph.root allNodes;
allNodes.${graph.root}.result
