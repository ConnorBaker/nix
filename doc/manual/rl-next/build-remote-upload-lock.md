---
synopsis: "Remote builds no longer wait for the upload lock when nothing needs uploading"
issues: []
prs: []
---

When Nix sends a build to a [remote builder](@docroot@/command-ref/conf-file.md#conf-builders), it now first asks the remote machine which of the build's inputs it is missing, and takes the upload lock for that machine only if some input has to be copied.
Previously every build sent to the same machine took the lock, so builds whose inputs were all present on the remote, or could be substituted there with [`builders-use-substitutes`](@docroot@/command-ref/conf-file.md#conf-builders-use-substitutes), still queued behind each other and behind other builds' uploads.
