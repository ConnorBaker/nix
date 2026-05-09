# Adversarial Review

Perform a rigorous adversarial review of the current change or plan.

Rules:
1. Do NOT speculate about causes — require evidence (logs, failing tests, code citations).
2. Flag any proposed fix that modifies a test instead of production code.
3. Flag any workaround not explicitly labeled as such.
4. Flag any semantic change (hash, dep-kind, invariant) that is not documented.
5. Flag tests that may pass vacuously (wrapper not firing, fall-through branches, assertions weakened).
6. Check for orphaned state (DB rows, cache entries) left by the change.
7. Read ALL relevant implementation files before concluding.

Output: numbered list of issues with severity (blocker/major/minor) and code citations.
