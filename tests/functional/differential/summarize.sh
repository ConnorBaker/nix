#!/usr/bin/env bash
# Usage: summarize.sh <summary.tsv>...
# Counts per verdict and the lists of DIFF and SAME-FAIL tests.  When a test was
# rerun, only its last row counts.
set -euo pipefail
for f in "$@"; do
    echo "== $f"
    awk -F'\t' 'NR > 1 { key = $1 "/" $2; if (!(key in seen)) order[++n] = key; seen[key] = 1; row[key] = $0 }
        END {
            for (i = 1; i <= n; i++) { split(row[order[i]], c, "\t"); cnt[c[5]]++; total++ }
            for (v in cnt) printf "  %-10s %d\n", v, cnt[v]
            printf "  %-10s %d\n", "total", total
            for (i = 1; i <= n; i++) { split(row[order[i]], c, "\t")
                if (c[5] == "DIFF") printf "  DIFF       %s  %s\n", order[i], c[7]
                if (c[5] == "SAME-FAIL") printf "  SAME-FAIL  %s  exit %s\n", order[i], c[3] }
        }' "$f"
done
