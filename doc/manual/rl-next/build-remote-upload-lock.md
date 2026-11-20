---
synopsis: "Remote builds no longer copy inputs to a builder one build at a time"
---

Build hooks on a machine took one lock per remote builder while copying a build's inputs there, including while asking the builder which inputs it already has.
As a result, the remote builds of every client on that machine copied their inputs one after another, and often only one or two builds were running on a builder with free slots.

Now only uploads of the same store path to the same builder exclude each other.
A build whose inputs the builder already has doesn't wait for other uploads at all.

Remote builders reached through `ssh://` now also keep a build's inputs from their garbage collector while the inputs are copied there, as `ssh-ng://` builders did.
A builder whose store is a Nix daemon does so only if that daemon is Nix 2.36 or later.
