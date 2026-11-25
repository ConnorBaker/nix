---
synopsis: "Builds wait for the builder to exit, not only for its log to close"
issues: [2176]
---

On Linux, a builder is no longer killed when it closes its standard output and standard error, so builders that close them just before exiting (as GNU coreutils programs do) no longer fail intermittently with `signal 9 (Killed)`.
A builder that closes its output and keeps running is now ended only by [`max-silent-time`](@docroot@/command-ref/conf-file.md#conf-max-silent-time) or [`timeout`](@docroot@/command-ref/conf-file.md#conf-timeout), which both default to no limit.
Without `pidfd_open()` (Linux before 5.3) and on other platforms, nothing changes.
