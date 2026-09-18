R""(

# Examples

* Sign a store path and its closure with a key:

  ```console
  # nix store sign --key-file ./secret-key --recursive /nix/store/y1x7ng5bmc9s8lqrf98brcpk1a7lbcl5-hello-2.10
  ```

# Description

This command signs each store path with the secret key in *file* and
adds the signatures to the store. Up to two signatures are added per
path, both by the same key: one over the current fingerprint (version
2, over the path's object hash), added when the path's description
carries an object hash; and one over the version-1 fingerprint (over
the path's NAR hash and size), which is the only form that Nix
versions before the object hash verify, added whenever the NAR size is
known. The NAR hash is taken from the path's description when it
asserts one (a binary cache's narinfo does); otherwise it is computed
by one walk of the path, so signing a local store costs one walk per
path. So the command works on any store, including a binary cache an
older Nix wrote, whose paths have no object hash and receive the
version-1 signature alone. A signature already present is not added
again.

Paths signed at build time (`secret-key-files`) carry a version-2
signature only; run this command on a binary cache they were copied
to, with the cache as `--store`, before older clients that require
signatures use it.

)""
