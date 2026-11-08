#!/usr/bin/env bash
# Summarize one nsys capture of the pufferl trainer: where the GPU time goes,
# which kernels dominate, and how much wall time is NOT covered by kernels
# (launch/latency-bound vs compute-bound).
#
# Usage: ./tools/nsys_report.sh build/nsys_bk [TOP_N]
set -euo pipefail

REP="${1:?usage: nsys_report.sh REPORT.nsys-rep [TOP_N]}"
TOP="${2:-25}"
case "$REP" in
    *.nsys-rep) ;;
    *) REP="$REP.nsys-rep" ;;
esac
[ -f "$REP" ] || { echo "no such report: $REP" >&2; exit 1; }
SQL="${REP%.nsys-rep}.sqlite"

echo "=== nsys stats: kernel summary (top $TOP by GPU time) ==="
nsys stats --report cuda_gpu_kern_sum --force-export=true -o /dev/stdout "$REP" 2>/dev/null \
    | head -$((TOP + 8)) || true

if [ ! -f "$SQL" ]; then
    nsys export --type sqlite --force-overwrite=true -o "${SQL%.sqlite}" "$REP" >/dev/null 2>&1 || true
fi
[ -f "$SQL" ] || { echo "no sqlite export for $REP" >&2; exit 1; }

echo
echo "=== kernel time by name (top $TOP) ==="
sqlite3 -header -column "$SQL" "
WITH kern AS (
    SELECT s.value AS name, (k.end - k.start) AS dur_ns
    FROM CUPTI_ACTIVITY_KIND_KERNEL k
    JOIN StringIds s ON k.shortName = s.id
), tot AS (SELECT SUM(dur_ns) AS t FROM kern)
SELECT name, COUNT(*) AS launches,
       ROUND(100.0 * SUM(dur_ns) / (SELECT t FROM tot), 1) AS pct,
       ROUND(SUM(dur_ns) / 1e6, 1) AS total_ms,
       ROUND(AVG(dur_ns) / 1e3, 2) AS avg_us,
       ROUND(MAX(dur_ns) / 1e3, 1) AS max_us
FROM kern GROUP BY name ORDER BY SUM(dur_ns) DESC LIMIT $TOP;
"

echo
echo "=== aggregate: GPU busy vs measured window ==="
sqlite3 -header -column "$SQL" "
WITH kern AS (SELECT start, end FROM CUPTI_ACTIVITY_KIND_KERNEL)
SELECT COUNT(*) AS launches,
       ROUND((MAX(end) - MIN(start)) / 1e6, 1) AS window_ms,
       ROUND(SUM(end - start) / 1e6, 1) AS busy_ms,
       ROUND(100.0 * SUM(end - start) / (MAX(end) - MIN(start)), 1) AS busy_pct
FROM kern;
"

echo
echo "=== per-stream aggregation: launch bubbles ==="
sqlite3 -header -column "$SQL" "
SELECT 'all_kernels' AS what, COUNT(*) AS n,
       ROUND(SUM(end - start) / 1e6, 1) AS busy_ms,
       ROUND(MAX(end) / 1e6, 1) AS last_end_ms
FROM CUPTI_ACTIVITY_KIND_KERNEL;
"

echo
echo "=== memcpy + memset volume ==="
sqlite3 -header -column "$SQL" "
SELECT 'memcpy' AS kind, COUNT(*) AS n, ROUND(SUM(bytes) / 1e6, 1) AS MB,
       ROUND(SUM(end - start) / 1e6, 2) AS ms
FROM CUPTI_ACTIVITY_KIND_MEMCPY
UNION ALL
SELECT 'memset', COUNT(*), ROUND(SUM(bytes) / 1e6, 1),
       ROUND(SUM(end - start) / 1e6, 2)
FROM CUPTI_ACTIVITY_KIND_MEMSET;
" || true
