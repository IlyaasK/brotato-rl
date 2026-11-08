#!/usr/bin/env bash
# Regenerate build/bench_trainer_report.md from every benchmark result directory.
# Usage: ./tools/bench_trainer_report_all.sh [OUTPUT.md]
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-build/bench_trainer_report.md}"

python3 tools/bench_trainer_report.py \
    build/bench_sps            tools/bench_trainer_runs.txt \
    build/bench_sps_gap        tools/bench_trainer_runs_gap.txt \
    build/bench_sps_repeat     tools/bench_trainer_runs_repeat.txt \
    build/bench_sps_fp32       tools/bench_trainer_runs_fp32.txt \
    build/bench_sps_admiral    tools/bench_trainer_runs_admiral.txt \
    build/bench_sps_big        tools/bench_trainer_runs_big.txt \
    build/bench_sps_br         tools/bench_trainer_runs_brotato.txt \
    build/bench_sps_brnew      tools/bench_trainer_runs_brotato_big.txt \
    build/bench_sps_breq       tools/bench_trainer_runs_brotato_eq.txt \
    build/bench_sps_brdeep     tools/bench_trainer_runs_brotato_deep.txt \
    build/bench_sps_reg_build_bench_brotato_bf16 tools/bench_trainer_runs_regress.txt \
    build/bench_sps_reg___puffer                 tools/bench_trainer_runs_regress.txt \
    build/bench_sps_ab_guards/old                build/bench_sps_ab_guards/runs.txt \
    build/bench_sps_ab_guards/new                build/bench_sps_ab_guards/runs.txt \
    > "$OUT"
echo "wrote $OUT ($(grep -c '^| ' "$OUT") table rows)"
