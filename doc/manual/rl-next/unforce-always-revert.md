---
synopsis: "New setting `unforce-always-revert`"
---

Where a build without Boehm GC frees evaluation memory in jobs (see the release note on freeing evaluation memory), a job by default keeps a value that an earlier job reverted and that it needed again, and a value that a later element of the same job needs, so that values shared between elements, such as the Nixpkgs library, are not evaluated anew by every job.

With the new setting [`unforce-always-revert`](@docroot@/command-ref/conf-file.md#conf-unforce-always-revert) set to `true`, these values are reverted at the end of every job as well, and evaluated again wherever they are next needed.
Less memory is kept, but evaluation can take more time.

The setting defaults to `false`, and has no effect where no jobs run: in builds with Boehm GC, and on systems other than 64-bit Linux.
