---
synopsis: Evaluation cache version bumped to v7 (drops failure caching)
---

The evaluation cache directory name changes from `eval-cache-v6` to `eval-cache-v7`. Existing v6 caches are not migrated; they will be re-populated on first use and can be deleted by hand if desired.

The bump accompanies a behaviour change: errors raised during attribute evaluation are no longer recorded in the cache. Previously, eval failures of certain shapes were memoised and replayed on subsequent lookups; now every failed attribute is re-evaluated. This removes a source of cache poisoning at the cost of some redundant work for repeatedly-failing attributes.
