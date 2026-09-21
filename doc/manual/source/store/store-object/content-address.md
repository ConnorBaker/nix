# Content-Addressing Store Objects

Just [like][fso-ca] [File System Objects][File System Object],
[Store Objects][Store Object] can also be [content-addressed](@docroot@/glossary.md#gloss-content-address),
unless they are [input-addressed](@docroot@/glossary.md#gloss-input-addressed-store-object).

For store objects, the content address we produce will take the form of a [Store Path] rather than regular hash.
In particular, the content-addressing scheme will ensure that the digest of the store path is solely computed from the

- file system object graph (the root one and its children, if it has any)
- references
- [store directory path](../store-path.md#store-directory-path)
- name

of the store object, and not any other information, which would not be an intrinsic property of that store object.

For the full specification of the algorithms involved, see the [specification of store path digests][sp-spec].

[File System Object]: ../file-system-object.md
[Store Object]: ../store-object.md
[Store Path]: ../store-path.md

## Content addressing each part of a store object

### File System Objects

With all currently-supported store object content-addressing methods, the file system object is always [content-addressed][fso-ca] first, and then that hash is incorporated into content address computation for the store object.

### References

#### References to other store objects

With all currently supported store object content addressing methods,
other objects are referred to by their regular (string-encoded-) [store paths][Store Path].

#### Self-references

Self-references however cannot be referred to by their path, because we are in the midst of describing how to compute that path!

> The alternative would require finding as hash function fixed point, i.e. the solution to an equation in the form
> ```
> digest = hash(..... || digest || ....)
> ```
> which is computationally infeasible.
> As far as we know, this is equivalent to finding a hash collision.

Instead we have a "has self-reference" boolean, which ends up affecting the digest:
In all currently-supported store object content-addressing methods, when hashing the file system object data, any occurrence of store object's own store path in the digested data is replaced with a [sentinel value](https://en.wikipedia.org/wiki/Sentinel_value).
The hashes of these modified input streams are used instead.

When validating the content address of a store object after the fact, the above process works as written.
However, when first creating the store object we don't know the store object's store path, as explained just above.
We therefore, strictly speaking, do not know what value we will be replacing with the sentinel value in the inputs to hash functions.
What instead happens is that the provisional store object --- the data from which we wish to create a store object --- is paired with a provisional "scratch" store path (that presumably was chosen when the data was created).
That provisional store path is instead what is replaced with the sentinel value, rather than the final store object which we do not yet know.

> **Design note**
>
> It is an informal property of content-addressed store objects that the choice of provisional store path should not matter.
> In other words, if a provisional store object is prepared in the same way except for the choice of provision store path, the provisional data need not be identical.
> But, after the sentinel value is substituted in place of each provisional store object's provision store path, the final so-normalized data *should* be identical.
>
> If, conversely, the data after this normalization process is still different, we'll compute a different content-address.
> The method of preparing the provisional self-referenced data has *failed* to be deterministic in the sense of not *leaking* the choice of provisional store path --- a choice which is supposed to be arbitrary --- into the final store object.
>
> This property is informal at this stage, because we are just describing store objects, which have no formal notion of their origin.
> Without that, we cannot rigorously define what it would mean for the process to be insufficiently deterministic.
> When we cover [derivations](@docroot@/store/derivation/index.md), we will be able to make this a formal property — not of content-addressed store objects themselves, but of the derivations that *produce* them.

### Name and Store Directory

These two items affect the digest in a way that is standard for store path digest computations and not specific to content-addressing.
Consult the [specification of store path digests][sp-spec] for further details.

## Content addressing Methods

For historical reasons, we don't support all features in all combinations.
Each currently supported method of content addressing chooses a single method of file system object hashing, and may offer some restrictions on references.
The names and store directories are unrestricted however.

### Flat { #method-flat }

This uses the corresponding [Flat](../file-system-object/content-address.md#serial-flat) method of file system object content addressing.

References are not supported: store objects with flat hashing *and* references can not be created.

### Text { #method-text }

This also uses the corresponding [Flat](../file-system-object/content-address.md#serial-flat) method of file system object content addressing.

References to other store objects are supported, but self-references are not.

This is the only store-object content-addressing method that is not named identically with a corresponding file system object method.
It is somewhat obscure, mainly used for "drv files"
(derivations serialized as store objects in their ["ATerm" file format](@docroot@/protocols/derivation-aterm.md#use-for-encoding-to-store-object)).
Prefer another method if possible.

### Nix Archive { #method-nix-archive }

This uses the corresponding [Nix Archive](../file-system-object/content-address.md#serial-nix-archive) method of file system object content addressing.

References (to other store objects and self-references alike) are supported so long as the hash algorithm is SHA-256, but not (neither kind) otherwise.

### Git { #method-git }

This uses the corresponding [Git](../file-system-object/content-address.md#git) method of file system object content addressing.

References are not supported.

Only SHA-256 is supported: the identifiers are those of [SHA-256-based Git](https://git-scm.com/docs/hash-function-transition), and the method is the one by which the store names every store object.
A SHA-1 identifier from an ordinary Git repository is not an address under this method; `nix hash path --mode git` gives the address of a tree.
Nix once admitted SHA-1 here as well, under the former `git-hashing` experimental feature; a fixed-output derivation that still declares `outputHashMode = "git"` with a SHA-1 hash is refused at instantiation, and `nix store add --mode git --hash-algo sha1` and `nix hash path --mode git --algo sha1` are refused, each with the remedy in the message.
A store object an older Nix addressed under SHA-1 is read as it is (its database row, a `.narinfo` naming it, a derivation naming its output), but cannot be copied into a store of this Nix, whose check of the address it cannot pass.

[fso-ca]: ../file-system-object/content-address.md
[sp-spec]: @docroot@/protocols/store-path.md
