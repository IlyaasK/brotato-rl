#!/usr/bin/env python3
"""Render a markdown report from bench_trainer_sps.py result directories.

Joins each run with the extra argv recorded in its runs file, re-parses the raw
trainer log (source of truth), and derives per-phase throughput:

  epoch       = total_agents * horizon samples
  SPS         = epoch / (rollout + train)     (agent-steps/s = env ticks/s)
  rollout SPS = epoch / rollout_time          (net forward + env step)
  train   SPS = epoch / train_time            (PPO replay: fwd+bwd+optimizer)
  step_us     = rollout_time / horizon        (per graph-launch latency)
  mb_ms       = train_time / minibatch count  (per learner step)

NOTE: with base.async=1 the rollout and learner run on separate streams and
overlap. The trainer's perf/rollout and perf/train accumulators then measure
elapsed wall time including contention, so their sum can exceed the epoch wall
time and the per-phase splits are only directly readable for base.async=0 (or
cudagraphs off) runs. The wall SPS (independently recomputed here from the
trainer's own step/uptime counters) is the trustworthy number.

Usage:
  python tools/bench_trainer_report.py \
      build/bench_sps tools/bench_trainer_runs.txt \
      build/bench_sps_fp32 tools/bench_trainer_runs_fp32.txt \
      build/bench_sps_admiral tools/bench_trainer_runs_admiral.txt
"""
import glob
import importlib.util
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "bench", os.path.join(HERE, "bench_trainer_sps.py"))
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)

DEFAULTS = {"vec.total_agents": 4096, "vec.num_buffers": 1, "policy.hidden_size": 32,
            "policy.num_layers": 4, "train.horizon": 32, "train.minibatch_size": 65536,
            "train.replay_ratio": 1.0}


def argval(args, key, default):
    for a in args:
        if a.startswith("--" + key + "="):
            return a.split("=", 1)[1]
    return default


def read_runs(path):
    out = {}
    if not os.path.exists(path):
        return out
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, _, args = line.partition("|")
        out[name.strip()] = args.split()
    return out


def distinct(frames):
    """Consecutive frames with an advancing step counter (the trainer re-prints its
    final snapshot several times)."""
    out = []
    for f in frames:
        if out and f.get("steps") == out[-1].get("steps"):
            continue
        out.append(f)
    return out


def plateau_sps(frames):
    """Median SPS over the interior of the run. The LAST window of a run that ends
    right after a log is short (the trainer logs unconditionally on its final epoch),
    which inflates a final-window SPS by up to 2x -- buffer/trainer phases are also
    re-printed unchanged there. Drop the first 2 and last 3 distinct frames."""
    uniq = distinct(frames)
    core = uniq[2:-3] if len(uniq) > 8 else uniq[1:-1]
    vals = [f["sps"] for f in core if f.get("sps")]
    if not vals:
        vals = [f["sps"] for f in frames if f.get("sps")]
    return statistics.median(vals) if vals else None


def last_delta(frames):
    """Last two distinct frames with a positive step delta (final snapshots repeat)."""
    uniq = distinct(frames)
    for a, b in zip(reversed(uniq[:-1]), reversed(uniq[1:])):
        d_steps = (b.get("steps") or 0) - (a.get("steps") or 0)
        d_up = ((b.get("uptime_ms") or 0) - (a.get("uptime_ms") or 0)) / 1000.0
        if d_steps > 0 and d_up > 0:
            return a, b, d_steps, d_up
    return None


def collect(out_dir, runs_file):
    args_by_name = read_runs(runs_file)
    rows = []
    for log in sorted(glob.glob(os.path.join(out_dir, "*.log"))):
        name = os.path.basename(log)[:-4]
        args = args_by_name.get(name, [])
        frames = bench.parse_frames(open(log).read())
        summary = bench.summarize(frames, 3)
        sps = summary.get("sps_median")
        ag = int(argval(args, "vec.total_agents", DEFAULTS["vec.total_agents"]))
        h = int(argval(args, "train.horizon", DEFAULTS["train.horizon"]))
        mb = int(argval(args, "train.minibatch_size", DEFAULTS["train.minibatch_size"]))
        rr = float(argval(args, "train.replay_ratio", DEFAULTS["train.replay_ratio"]))
        row = {
            "run": name, "agents": ag, "horizon": h, "mb": mb, "rr": rr,
            "hidden": int(argval(args, "policy.hidden_size", DEFAULTS["policy.hidden_size"])),
            "layers": int(argval(args, "policy.num_layers", DEFAULTS["policy.num_layers"])),
            "params": summary.get("params"), "vram": summary.get("vram_used_gb"),
            "perf": summary.get("perf"), "endless_wave": summary.get("endless_wave"),
            "sps": plateau_sps(frames) or sps,
            "sps_final_window": sps,
            "sps_life": summary.get("sps_lifetime"),
            "flags": " ".join(a for a in args if a.startswith(
                ("--base", "--vec.num_buffers", "--train.replay_ratio"))),
            "file": log,
        }
        uniq = distinct(frames)
        delta = last_delta(uniq[:-3] if len(uniq) > 8 else uniq)
        if delta and sps:
            a, b, d_steps, d_up = delta
            epoch = ag * h
            d_ep = (b.get("epoch") or 0) - (a.get("epoch") or 0)
            # Phase times are GPU-busy ms accumulated since frame a; async overlaps
            # them, so wall time can be less than roll+train.
            row["sps_delta"] = d_steps / d_up
            row["epoch_ms"] = d_up * 1000.0 / d_ep if d_ep else None
            if d_ep:
                row["roll_epoch_ms"] = (b.get("eval_rollout_ms") or 0) / d_ep
                row["train_epoch_ms"] = (b.get("train_ms") or 0) / d_ep
                row["roll_sps"] = epoch / row["roll_epoch_ms"] * 1000.0 \
                    if row["roll_epoch_ms"] else None
                row["train_sps"] = epoch / row["train_epoch_ms"] * 1000.0 \
                    if row["train_epoch_ms"] else None
                row["step_us"] = row["roll_epoch_ms"] / h * 1000.0
                row["net_epoch_ms"] = (b.get("eval_model_ms") or 0) / d_ep
                row["env_epoch_ms"] = (b.get("eval_env_ms") or 0) / d_ep
                n_mb = max(1, int(rr * epoch / mb))
                row["mb_ms"] = row["train_epoch_ms"] / n_mb
                phases = [t for t in (row["roll_epoch_ms"], row["train_epoch_ms"]) if t]
                if phases:
                    row["ideal_sps"] = epoch / max(phases) * 1000.0
                    row["overlap_pct"] = 100.0 * row["sps_delta"] / row["ideal_sps"]
        rows.append(row)
    return rows


def fmt(v, scale=1.0, nd=2, suffix=""):
    return "-" if v is None else ("%.*f%s" % (nd, v / scale, suffix))


HEADER = ("| run | hid/L | agents | horizon | minibatch | replay | SPS (M/s) | "
          "final-window (M/s) | lifetime (M/s) | "
          "roll SPS (M/s) | train SPS (M/s) | roll ms/ep | train ms/ep | net ms/ep | "
          "env ms/ep | step us | mb ms | ideal/overlap | env/perf | VRAM GB | flags |")
RULE = "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"


def table(rows, title):
    print("\n### %s\n" % title)
    print("SPS = median over interior dashboard frames; final-window/lifetime are the "
          "last-window and whole-run averages (the last window of a completed run is "
          "short and reads high).\n")
    print(HEADER)
    print(RULE)
    for r in sorted(rows, key=lambda r: (r["hidden"], r["layers"], r["horizon"],
                                         r["agents"], r["mb"])):
        print("| %s | %d/%d | %d | %d | %d | %g | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["run"], r["hidden"], r["layers"], r["agents"], r["horizon"], r["mb"],
            r["rr"], fmt(r["sps"], 1e6), fmt(r.get("sps_final_window"), 1e6),
            fmt(r.get("sps_life"), 1e6),
            fmt(r.get("roll_sps"), 1e6), fmt(r.get("train_sps"), 1e6),
            fmt(r.get("roll_epoch_ms"), 1.0, 1), fmt(r.get("train_epoch_ms"), 1.0, 1),
            fmt(r.get("net_epoch_ms"), 1.0, 2), fmt(r.get("env_epoch_ms"), 1.0, 2),
            fmt(r.get("step_us"), 1.0, 1), fmt(r.get("mb_ms"), 1.0, 2),
            "%s/%s%%" % (fmt(r.get("ideal_sps"), 1e6, 2), fmt(r.get("overlap_pct"), 1.0, 0)),
            fmt(r.get("perf"), 1.0, 2), fmt(r["vram"], 1.0, 1), r["flags"]))


def main():
    argv = sys.argv[1:]
    if len(argv) % 2:
        print(__doc__)
        return 2
    for i in range(0, len(argv), 2):
        out_dir, runs_file = argv[i], argv[i + 1]
        table(collect(out_dir, runs_file), "%s (runs: %s)" % (out_dir, runs_file))
    return 0


if __name__ == "__main__":
    sys.exit(main())
