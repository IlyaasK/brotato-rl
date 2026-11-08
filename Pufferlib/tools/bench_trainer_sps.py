#!/usr/bin/env python3
"""Steady-state SPS harness for the native pufferl trainer.

Runs a built trainer binary (`./build.sh ENV OUT --cu`) repeatedly with
different `--section.key=value` overrides, parses the last dashboard frame(s)
printed by the trainer, and reports agent-steps/second plus the rollout/train
time split.

Usage:
    python tools/bench_trainer_sps.py \
        --bin build/bench_breakout_bf16 \
        --runs tools/bench_trainer_runs.txt \
        --out build/bench_sps \
        [--timeout 20] [--window 3]

Each non-comment line of --runs is:
    name | --vec.total_agents=65536 --train.horizon=64 ...

Output: <out>/<name>.log for every run, plus <out>/results.json and a markdown
table on stdout. Only the trainer's own steady-state loops are exercised; the
run is cut with SIGTERM after --timeout seconds, so train.total_timesteps is
left at its config default.
"""
import argparse
import glob
import json
import os
import re
import signal
import statistics
import subprocess
import sys
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
FRAME_SEP = "\u256d"  # top-left corner of a dashboard box
MID = re.compile(r"(Evaluate|Model|Env|Copy|Train|Model|Misc)\s+(\S+)\s+(\d+)%")
NUM = re.compile(r"([0-9]*\.?[0-9]+)\s*([KMB])?", re.IGNORECASE)


def parse_num(text):
    """'9.6M' -> 9600000.0, '424.4K' -> 424400.0."""
    m = NUM.match(text.strip())
    if not m:
        return None
    val = float(m.group(1))
    suffix = (m.group(2) or "").upper()
    return val * {"": 1, "K": 1e3, "M": 1e6, "B": 1e9}[suffix]


def parse_duration(text):
    """'868ms', '4s 301ms', '1d 5h 23m 12s' -> milliseconds."""
    t = text.strip()
    if not t:
        return None
    if re.fullmatch(r"[0-9.]+(ms|us|\u00b5s|s)", t):
        if t.endswith("ms"):
            return float(t[:-2])
        if t.endswith("s"):
            return float(t[:-1]) * 1000.0
        return float(re.sub(r"(us|\u00b5s)$", "", t)) / 1000.0
    ms = 0.0
    found = re.findall(r"([0-9.]+)(ms|d|h|m|s)", t)
    if not found:
        return None
    for value, unit in found:
        ms += float(value) * {"d": 86400000.0, "h": 3600000.0,
                              "m": 60000.0, "s": 1000.0, "ms": 1.0}[unit]
    return ms


MID_LABELS = ("Evaluate", "Model", "Env", "Copy", "Train", "Model", "Misc")
MID_NAMES = ("eval_rollout_ms", "eval_model_ms", "eval_env_ms", "eval_copy_ms",
             "train_ms", "train_model_ms", "train_misc_ms")
LEFT_KEYS = {"SPS": ("sps", parse_num), "Steps": ("steps", parse_num),
             "Params": ("params", parse_num), "Uptime": ("uptime_ms", parse_duration),
             "Epoch": ("epoch", int)}


def parse_frame(chunk):
    """One dashboard box -> dict. Columns are padded with 2+ spaces, so a row
    splits into [left_label, left_value, mid_label, mid_value, mid_pct,
    right_label, right_value] with multi-word values kept whole."""
    frame = {}
    mid_seq = []
    for line in chunk.splitlines():
        line = line.strip().strip("\u2502 \t")
        if not line:
            continue
        tokens = re.split(r"\s{2,}", line)
        for i, tok in enumerate(tokens[:-1]):
            key = LEFT_KEYS.get(tok)
            if key and key[0] not in frame:
                try:
                    frame[key[0]] = key[1](tokens[i + 1])
                except (ValueError, TypeError):
                    pass
            if tok in MID_LABELS:
                ms = parse_duration(tokens[i + 1])
                if ms is not None:
                    mid_seq.append((tok, ms, tokens[i + 2]))
        m = re.search(r"VRAM:\s*([0-9.]+)/([0-9.]+)G", line)
        if m:
            frame["vram_used_gb"] = float(m.group(1))
        # Compact one-line mode (tiny TTY): "env=x steps=1.2M SPS=9.6M ...".
        for key, pat in (("sps", r"SPS=([0-9.]+[KMB]?)"),
                         ("steps", r"steps=([0-9.]+[KMB]?)")):
            m = re.search(pat, line)
            if m and key not in frame:
                frame[key] = parse_num(m.group(1))
        # Footer stats row: "| perf 0.023 score 19.613 |" (env/perf = wave reached
        # for Brotato; also env/<key> lines in the full dashboard).
        m = re.search(r"\bperf\s+(-?[0-9.]+)", line)
        if m and "perf" not in frame:
            frame["perf"] = float(m.group(1))
        m = re.search(r"\bepisode_length\s+(-?[0-9.]+)", line)
        if m and "episode_length" not in frame:
            frame["episode_length"] = float(m.group(1))
        m = re.search(r"\bscore\s+(-?[0-9.]+)", line)
        if m and "score" not in frame:
            frame["score"] = float(m.group(1))
        m = re.search(r"\bendless_wave\s+(-?[0-9.]+)", line)
        if m and "endless_wave" not in frame:
            frame["endless_wave"] = float(m.group(1))
    # Middle column renders in fixed order; anchor on Evaluate, then take 7.
    labels = [lbl for lbl, _, _ in mid_seq]
    start = labels.index("Evaluate") if "Evaluate" in labels else 0
    for (_, ms, pct), name in zip(mid_seq[start:], MID_NAMES):
        if name in frame:
            continue
        frame[name] = ms
        pct_digits = pct.rstrip("%")
        frame[name.replace("_ms", "_pct")] = int(pct_digits) \
            if pct_digits.isdigit() else None
    return frame


def parse_frames(stdout):
    """Return dashboard frames as dicts (last frame last)."""
    clean = ANSI.sub("", stdout)
    frames = []
    for chunk in clean.split(FRAME_SEP)[1:]:
        frame = parse_frame(chunk)
        if frame.get("sps"):
            frames.append(frame)
    return frames


def distinct(frames):
    """Consecutive frames with an advancing step counter (the trainer re-prints its
    final snapshot)."""
    out = []
    for f in frames:
        if out and f.get("steps") == out[-1].get("steps"):
            continue
        out.append(f)
    return out


def summarize(frames, window):
    if not frames:
        return {}
    tail = frames[-window:]
    uniq = distinct(frames)
    core = uniq[2:-3] if len(uniq) > 8 else uniq[1:-1]
    plateau_vals = [f["sps"] for f in core if f.get("sps")] or \
                   [f["sps"] for f in frames if f.get("sps")]
    out = {
        "sps_last": frames[-1]["sps"],
        "sps_median": statistics.median(f["sps"] for f in tail),
        "sps_plateau": statistics.median(plateau_vals) if plateau_vals else None,
        "frames": len(frames),
    }
    for key in ("steps", "uptime_ms", "params", "vram_used_gb", "epoch",
                "eval_rollout_ms", "train_ms", "train_model_ms", "train_misc_ms",
                "eval_model_ms", "eval_env_ms", "eval_copy_ms"):
        vals = [f[key] for f in tail if key in f]
        if vals:
            out[key] = statistics.median(vals)
    # Episode stats are only printed on frames with completed episodes (n>0), and the
    # trainer's final snapshots omit them; take the last frame that carries them.
    for key in ("perf", "episode_length", "score", "endless_wave"):
        val = None
        for f in reversed(frames):
            if key in f and f[key] is not None:
                val = f[key]
                break
        if val is not None:
            out[key] = val
    # Cumulative counters: use the final frame, not a median.
    last = frames[-1]
    if last.get("steps") and last.get("uptime_ms"):
        out["sps_lifetime"] = last["steps"] / (last["uptime_ms"] / 1000.0)
    total = (out.get("eval_rollout_ms") or 0) + (out.get("train_ms") or 0)
    if total > 0:
        out["rollout_pct"] = 100.0 * (out.get("eval_rollout_ms") or 0) / total
        out["train_pct"] = 100.0 * (out.get("train_ms") or 0) / total
    return out


def read_runs(path):
    runs = []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, _, args = line.partition("|")
            runs.append((name.strip(), args.split()))
    return runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", help="built trainer binary (omitted with --parse-only)")
    ap.add_argument("--runs", help="runs file (omitted with --parse-only)")
    ap.add_argument("--out", default="build/bench_sps")
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--window", type=int, default=3)
    ap.add_argument("--only", default=None, help="substring filter on run name")
    ap.add_argument("--globals", default="",
                    help="extra argv appended to every run (space separated)")
    ap.add_argument("--parse-only", action="store_true",
                    help="re-parse <out>/*.log instead of running anything")
    args = ap.parse_args()
    globals_extra = args.globals.split()

    os.makedirs(args.out, exist_ok=True)
    results = []
    if args.parse_only:
        for log_path in sorted(glob.glob(os.path.join(args.out, "*.log"))):
            name = os.path.basename(log_path)[:-4]
            if args.only and args.only not in name:
                continue
            summary = summarize(parse_frames(open(log_path).read()), args.window)
            summary.update({"name": name, "log": log_path})
            results.append(summary)
    else:
        for name, extra in read_runs(args.runs):
            if args.only and args.only not in name:
                continue
            cmd = [args.bin, "train"] + extra + globals_extra
            log_path = os.path.join(args.out, name + ".log")
            started = time.time()
            with open(log_path, "w") as log:
                proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                                        start_new_session=True)
                try:
                    proc.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGTERM)
                    try:
                        proc.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait()
            with open(log_path) as fh:
                stdout = fh.read()
            summary = summarize(parse_frames(stdout), args.window)
            summary.update({"name": name, "args": extra, "cmd": " ".join(cmd),
                            "wall_s": round(time.time() - started, 1),
                            "log": log_path})
            results.append(summary)
            sps = summary.get("sps_plateau") or summary.get("sps_median")
            print("%-34s SPS=%s  rollout=%s%% train=%s%%  %s" % (
                name,
                ("%.3fM" % (sps / 1e6)) if sps else "n/a",
                ("%.0f" % summary["rollout_pct"]) if "rollout_pct" in summary else "-",
                ("%.0f" % summary["train_pct"]) if "train_pct" in summary else "-",
                " ".join(extra)), flush=True)

    json_name = "results_reparse.json" if args.parse_only else "results.json"
    with open(os.path.join(args.out, json_name), "w") as fh:
        json.dump(results, fh, indent=1, sort_keys=True)

    print("\n| run | SPS (plateau) | SPS last window | SPS lifetime | rollout ms | train ms | train model ms | env ms | perf | params | VRAM GB |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for r in results:
        def fmt(key, scale=1.0, nd=1):
            val = r.get(key)
            return "-" if val is None else ("%.*f" % (nd, val / scale))
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["name"],
            fmt("sps_plateau", 1e6, 2) + "M" if "sps_plateau" in r else "-",
            fmt("sps_median", 1e6, 2) + "M" if "sps_median" in r else "-",
            fmt("sps_lifetime", 1e6, 2) + "M" if "sps_lifetime" in r else "-",
            fmt("eval_rollout_ms"), fmt("train_ms"), fmt("train_model_ms"),
            fmt("eval_env_ms"), fmt("perf", 1.0, 2),
            fmt("params", 1e3, 1) + "K" if "params" in r else "-",
            fmt("vram_used_gb")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
