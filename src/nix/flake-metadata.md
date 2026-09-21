R""(

# Examples

* Show what `dwarffs`, pinned to a revision, resolves to:

  ```console
  # nix flake metadata dwarffs/d181d714fd36eb06f4992a1997cd5601e26db8f5
  Resolved URL:  github:edolstra/dwarffs/d181d714fd36eb06f4992a1997cd5601e26db8f5
  Locked URL:    github:edolstra/dwarffs/d181d714fd36eb06f4992a1997cd5601e26db8f5?treeHash=sha256-IjFBbAAu9cCMmlpnVX1Vv73aACKrZ0ZTt9JFQRdNLvQ%3D
  Description:   A filesystem that fetches DWARF debug info from the Internet on demand
  Path:          /nix/store/rma6brjxd5cwsbxhqinhm39xpglq9260-source
  Revision:      d181d714fd36eb06f4992a1997cd5601e26db8f5
  Last modified: 2020-08-11 06:45:08
  Fingerprint:   4958c1191c84047846402de4b80d7443470fd2e96ef65306ce100fb91e024e6a
  Inputs:
  ├───nix: github:NixOS/nix/1ab9da915422405452118ebb17b88cdfc90b1e10?narHash=sha256-M801IExREv1T9F%2BK6YcCFERBFZ3%2B6ShwzAR2K7xvExA%3D (2020-07-07 12:38:57)
  │   └───nixpkgs: github:NixOS/nixpkgs/70717a337f7ae4e486ba71a500367cad697e5f09?narHash=sha256-oVXv4xAnDJB03LvZGbC72vSVlIbbJr8tpjEW5o/Fdek%3D (2020-06-08 16:22:16)
  └───nixpkgs follows input 'nix/nixpkgs'
  ```

  (The inputs' lock file predates `treeHash`, so they still show `narHash`; the flake's own locked URL carries `treeHash`.)

* Show the same information in JSON format:

  ```console
  # nix flake metadata dwarffs/d181d714fd36eb06f4992a1997cd5601e26db8f5 --json | jq .
  {
    "description": "A filesystem that fetches DWARF debug info from the Internet on demand",
    "fingerprint": "4958c1191c84047846402de4b80d7443470fd2e96ef65306ce100fb91e024e6a",
    "lastModified": 1597153508,
    "locked": {
      "__final": true,
      "lastModified": 1597153508,
      "owner": "edolstra",
      "repo": "dwarffs",
      "rev": "d181d714fd36eb06f4992a1997cd5601e26db8f5",
      "treeHash": "sha256-IjFBbAAu9cCMmlpnVX1Vv73aACKrZ0ZTt9JFQRdNLvQ=",
      "type": "github"
    },
    "locks": { ... },
    "original": {
      "id": "dwarffs",
      "rev": "d181d714fd36eb06f4992a1997cd5601e26db8f5",
      "type": "indirect"
    },
    "originalUrl": "flake:dwarffs/d181d714fd36eb06f4992a1997cd5601e26db8f5",
    "path": "/nix/store/rma6brjxd5cwsbxhqinhm39xpglq9260-source",
    "resolved": {
      "owner": "edolstra",
      "repo": "dwarffs",
      "rev": "d181d714fd36eb06f4992a1997cd5601e26db8f5",
      "type": "github"
    },
    "resolvedUrl": "github:edolstra/dwarffs/d181d714fd36eb06f4992a1997cd5601e26db8f5",
    "revision": "d181d714fd36eb06f4992a1997cd5601e26db8f5",
    "url": "github:edolstra/dwarffs/d181d714fd36eb06f4992a1997cd5601e26db8f5?treeHash=sha256-IjFBbAAu9cCMmlpnVX1Vv73aACKrZ0ZTt9JFQRdNLvQ%3D"
  }
  ```

# Description

This command shows information about the flake specified by the flake
reference *flake-url*. It resolves the flake reference using the
[flake registry](./nix3-registry.md), fetches it, and prints some meta
data. This includes:

* `Resolved URL`: If *flake-url* is a flake identifier, then this is
  the flake reference that specifies its actual location, looked up in
  the flake registry.

* `Locked URL`: A flake reference that contains a commit or content
  hash and thus uniquely identifies a specific flake version.

* `Description`: A one-line description of the flake, taken from the
  `description` field in `flake.nix`.

* `Path`: The store path containing the source code of the flake.

* `Revision`: The Git or Mercurial commit hash of the locked flake.

* `Revisions`: The number of ancestors of the Git or Mercurial commit
  of the locked flake. Note that this is not available for `github`
  flakes.

* `Last modified`: For Git or Mercurial flakes, this is the commit
  time of the commit of the locked flake; for tarball flakes, it's the
  most recent timestamp of any file inside the tarball.

* `Inputs`: The flake inputs with their corresponding lock file
  entries.

With `--json`, the output is a JSON object with the following fields:

* `original` and `originalUrl`: The flake reference specified by the
  user (*flake-url*) in attribute set and URL representation.

* `resolved` and `resolvedUrl`: The resolved flake reference (see
  above) in attribute set and URL representation.

* `locked` and `lockedUrl`: The locked flake reference (see above) in
  attribute set and URL representation.

* `description`: See `Description` above.

* `path`: See `Path` above.

* `revision`: See `Revision` above.

* `revCount`: See `Revisions` above.

* `lastModified`: See `Last modified` above.

* `locks`: The contents of `flake.lock`.

)""
