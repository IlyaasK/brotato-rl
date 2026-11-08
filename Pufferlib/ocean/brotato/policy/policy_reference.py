#!/usr/bin/env python3
"""Independent reference implementation and validation harness for the exported
Brotato policy artifact.

This module is deliberately independent of the GDScript inference class: it
re-derives the forward pass straight from the equations in the native trainer
sources (src/algo.cu:218-244 ``mingru_gate`` with ``sigmoid`` at src/algo.cu:207
and ``lerp`` at src/algo.cu:212; ``encoder_forward``/``decoder_forward`` at
src/algo.cu:575-584/635-643 driving ``puf_mm`` at src/algo.cu:137-146; the value
column is the last decoder column, src/pufferl.cu:702).

It provides three commands:

    fixture   create a deterministic synthetic checkpoint, export it with the
              real exporter, and write a fixture of observations plus expected
              outputs computed by this reference;
    check     compare the JSON emitted by the Godot validation script against
              the fixture's expected outputs;
    probe     write a fixture for an artifact that already exists (e.g. a
              trained one), so the Godot forward can be exercised on it.

The arithmetic mirrors the GDScript/PoolRealArray contract: inputs and weights
are float32, accumulation happens in double, and every value stored back into a
float32 array is rounded to float32 (``f32``).  No numpy is required.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import struct
import sys

import export_policy


def f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def sigmoid_f(x: float) -> float:
    z = math.exp(-abs(x))
    if x >= 0.0:
        return 1.0 / (1.0 + z)
    return z / (1.0 + z)


def lerp_f(a: float, b: float, w: float) -> float:
    diff = b - a
    if abs(w) < 0.5:
        return a + w * diff
    return b - diff * (1.0 - w)


def linear(x, w, in_dim: int, out_dim: int):
    """out = x @ W^T with W stored row-major [out_dim, in_dim] (puf_mm)."""
    y = []
    for o in range(out_dim):
        acc = 0.0
        base = o * in_dim
        for i in range(in_dim):
            acc += x[i] * w[base + i]
        y.append(f32(acc))
    return y


class Reference:
    """Recurrent reference policy loaded from an artifact dict."""

    def __init__(self, artifact: dict):
        layout = artifact["layout"]
        self.obs_size = int(layout["obs_size"])
        self.hidden = int(layout["hidden"])
        self.layers = int(layout["layers"])
        self.actions = int(layout["actions"])
        tensors = {t["name"]: t for t in artifact["tensors"]}

        def decode(name):
            raw = _b64(tensors[name]["data"])
            return list(struct.unpack("<%df" % (len(raw) // 4), raw))

        self.enc = decode("encoder.weight")
        self.dec = decode("decoder.weight")
        self.mingru = [decode("mingru.%d.weight" % i) for i in range(self.layers)]
        self.reset()

    def reset(self) -> None:
        self.state = [0.0] * (self.layers * self.hidden)

    def step(self, obs, mask=None):
        if len(obs) != self.obs_size:
            raise ValueError("observation length %d, expected %d"
                             % (len(obs), self.obs_size))
        if mask is not None and len(mask) != self.actions:
            raise ValueError("action mask length %d, expected %d"
                             % (len(mask), self.actions))
        x = linear(obs, self.enc, self.obs_size, self.hidden)
        for l in range(self.layers):
            combined = linear(x, self.mingru[l], self.hidden, 3 * self.hidden)
            out = []
            base = l * self.hidden
            for hi in range(self.hidden):
                hid_val = combined[hi]
                gate_val = combined[self.hidden + hi]
                proj_val = combined[2 * self.hidden + hi]
                state_val = self.state[base + hi]
                gate_sig = sigmoid_f(gate_val)
                tilde = hid_val + 0.5 if hid_val >= 0.0 else sigmoid_f(hid_val)
                mingru_out = f32(lerp_f(state_val, tilde, gate_sig))
                proj_sig = sigmoid_f(proj_val)
                self.state[base + hi] = mingru_out
                out.append(f32(proj_sig * mingru_out + (1.0 - proj_sig) * x[hi]))
            x = out
        dec = linear(x, self.dec, self.hidden, self.actions + 1)
        logits = dec[:self.actions]
        value = dec[self.actions]
        # Masked-off logits become MASKED_LOGIT before the argmax, exactly as the
        # trainer does before its softmax (src/algo.cu:1364-1371).
        masked_logits = list(logits)
        if mask is not None:
            for a in range(self.actions):
                if not mask[a]:
                    masked_logits[a] = export_policy.MASKED_LOGIT
        action = max(range(self.actions), key=lambda a: masked_logits[a])
        return {"logits": logits, "value": value, "action": action,
                "masked_logits": masked_logits}


def _b64(text: str) -> bytes:
    import base64
    return base64.b64decode(text)


def _write_checkpoint(path: str, layout, rng: random.Random) -> None:
    """Write a checkpoint in the trainer's own on-disk format (test fixture)."""
    export_policy.write_trainer_checkpoint(
        path, layout, lambda _name, _i: f32(rng.uniform(-0.5, 0.5)))


def _fixture_dict(artifact: dict, artifact_name: str, seq_lens, seed: int) -> dict:
    ref = Reference(artifact)
    rng = random.Random(seed)
    sequences = []
    for seq_i, seq_len in enumerate(seq_lens):
        obs_seq = [[f32(rng.uniform(-1.0, 1.0)) for _ in range(ref.obs_size)]
                   for _ in range(seq_len)]
        # Exercise the action mask: the odd-numbered sequences carry a mask with
        # at least one legal and one masked action per step (the trainer's
        # -1e4 substitution, src/algo.cu:1364-1371), the even ones omit it.
        masks = None
        if seq_i % 2 == 1:
            masks = []
            for _ in range(seq_len):
                mask = [1] * ref.actions
                for a in rng.sample(range(ref.actions),
                                    max(1, ref.actions // 3)):
                    mask[a] = 0
                mask[rng.randrange(ref.actions)] = 1
                masks.append(mask)
        ref.reset()
        expected = [ref.step(obs_seq[t], None if masks is None else masks[t])
                    for t in range(seq_len)]
        sequence = {"obs": obs_seq, "expected": expected}
        if masks is not None:
            sequence["mask"] = masks
        sequences.append(sequence)
    return {
        "obs_size": ref.obs_size,
        "hidden": ref.hidden,
        "layers": ref.layers,
        "actions": ref.actions,
        "masked_logit": export_policy.MASKED_LOGIT,
        "artifact": artifact_name,
        "sequences": sequences,
    }


def make_fixture(out_dir: str, obs_size: int | None, hidden: int, layers: int,
                 actions: int | None, seed: int = 1234) -> dict:
    """Write a forward-pass fixture + its artifact + checkpoint.

    ``obs_size``/``actions`` default to the LIVE core schema (the artifact must
    declare exactly that schema; the exporter has no opt-out), while ``hidden``
    and ``layers`` are free network shape for the arithmetic comparison.
    """
    core = export_policy.read_core_schema()
    if obs_size is None:
        obs_size = core.obs_size
    if actions is None:
        actions = core.constants["actions"]
    os.makedirs(out_dir, exist_ok=True)
    rng = random.Random(seed)
    layout = export_policy.build_layout(obs_size, hidden, layers, actions, False)
    checkpoint = os.path.join(out_dir, "fixture_checkpoint.bin")
    _write_checkpoint(checkpoint, layout, rng)
    artifact_path = os.path.join(out_dir, "policy.json")
    artifact = export_policy.export(checkpoint, artifact_path, obs_size, hidden,
                                    layers, actions, continuous=False)

    fixture = _fixture_dict(artifact, os.path.basename(artifact_path),
                            (1, 3, 5), seed + 1)
    fixture_path = os.path.join(out_dir, "fixture.json")
    with open(fixture_path, "w") as handle:
        json.dump(fixture, handle)
    return {"artifact": artifact_path, "fixture": fixture_path}


def make_probe(artifact_path: str, out_dir: str, seq_len: int = 4,
               seed: int = 5678) -> dict:
    """Write a fixture that exercises an already-exported artifact."""
    with open(artifact_path) as handle:
        artifact = json.load(handle)
    os.makedirs(out_dir, exist_ok=True)
    fixture = _fixture_dict(artifact, os.path.basename(artifact_path),
                            (1, seq_len), seed)
    fixture_path = os.path.join(out_dir, "fixture.json")
    with open(fixture_path, "w") as handle:
        json.dump(fixture, handle)
    return {"artifact": artifact_path, "fixture": fixture_path}


def _close(a: float, b: float, rtol: float, atol: float) -> bool:
    return abs(a - b) <= atol + rtol * abs(b)


def compare(fixture: dict, produced: dict, rtol: float, atol: float) -> list:
    """Return a list of human-readable mismatches (empty == pass)."""
    problems = []
    if produced.get("obs_size") != fixture["obs_size"]:
        problems.append("Godot reported obs_size %r, fixture %r"
                        % (produced.get("obs_size"), fixture["obs_size"]))
    got_seqs = produced.get("sequences", [])
    if len(got_seqs) != len(fixture["sequences"]):
        problems.append("Godot ran %d sequences, fixture has %d"
                        % (len(got_seqs), len(fixture["sequences"])))
        return problems
    for si, (exp_seq, got_seq) in enumerate(zip(fixture["sequences"], got_seqs)):
        exp_steps = exp_seq["expected"]
        got_steps = got_seq.get("steps", [])
        if len(got_steps) != len(exp_steps):
            problems.append("seq %d: %d steps, expected %d"
                            % (si, len(got_steps), len(exp_steps)))
            continue
        for t, (exp, got) in enumerate(zip(exp_steps, got_steps)):
            if not got.get("ok", False):
                problems.append("seq %d step %d: Godot error %r"
                                % (si, t, got.get("error")))
                continue
            if got.get("action") != exp["action"]:
                problems.append("seq %d step %d: action %r != %r"
                                % (si, t, got.get("action"), exp["action"]))
            if not _close(float(got.get("value", 0.0)), exp["value"], rtol, atol):
                problems.append("seq %d step %d: value %r != %r"
                                % (si, t, got.get("value"), exp["value"]))
            got_logits = got.get("logits", [])
            if len(got_logits) != len(exp["logits"]):
                problems.append("seq %d step %d: %d logits, expected %d"
                                % (si, t, len(got_logits), len(exp["logits"])))
                continue
            for a, (gl, el) in enumerate(zip(got_logits, exp["logits"])):
                if not _close(float(gl), el, rtol, atol):
                    problems.append(
                        "seq %d step %d logit %d: %r != %r (rtol=%g atol=%g)"
                        % (si, t, a, gl, el, rtol, atol))
            # Masked logits carry the decision the mask forces (masked ids are
            # MASKED_LOGIT); comparing them proves the mask was applied by the
            # forward, not only that the argmax happened to agree.
            if "masked_logits" in exp and "masked_logits" in got:
                got_masked = got["masked_logits"]
                if len(got_masked) != len(exp["masked_logits"]):
                    problems.append("seq %d step %d: %d masked logits, expected %d"
                                    % (si, t, len(got_masked),
                                       len(exp["masked_logits"])))
                    continue
                for a, (gm, em) in enumerate(zip(got_masked, exp["masked_logits"])):
                    if not _close(float(gm), em, rtol, atol):
                        problems.append(
                            "seq %d step %d masked logit %d: %r != %r "
                            "(rtol=%g atol=%g)"
                            % (si, t, a, gm, em, rtol, atol))
    # Failure handling reported by the Godot validator.
    guard = produced.get("guards", {})
    if guard.get("wrong_size_ok") is not False:
        problems.append("wrong-size observation was not rejected (ok=%r)"
                        % guard.get("wrong_size_ok"))
    if guard.get("nonfinite_ok") is not False:
        problems.append("non-finite observation was not rejected (ok=%r)"
                        % guard.get("nonfinite_ok"))
    if guard.get("state_preserved") is not True:
        problems.append("rejected observation mutated recurrent state (state_preserved=%r)"
                        % guard.get("state_preserved"))
    return problems


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    pfix = sub.add_parser("fixture", help="write fixture + artifact + checkpoint")
    pfix.add_argument("--out-dir", required=True)
    pfix.add_argument("--obs-size", type=int, default=None,
                      help="defaults to the live core schema width")
    pfix.add_argument("--hidden", type=int, default=8)
    pfix.add_argument("--layers", type=int, default=2)
    pfix.add_argument("--actions", type=int, default=None,
                      help="defaults to the live core action count")
    pfix.add_argument("--seed", type=int, default=1234)

    pchk = sub.add_parser("check", help="compare Godot output with the fixture")
    pchk.add_argument("--fixture", required=True)
    pchk.add_argument("--godot", required=True)
    pchk.add_argument("--rtol", type=float, default=1e-5)
    pchk.add_argument("--atol", type=float, default=1e-4)

    pprobe = sub.add_parser("probe", help="fixture from an existing artifact")
    pprobe.add_argument("--artifact", required=True)
    pprobe.add_argument("--out-dir", required=True)
    pprobe.add_argument("--seq-len", type=int, default=4)
    pprobe.add_argument("--seed", type=int, default=5678)
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "fixture":
        paths = make_fixture(args.out_dir, args.obs_size, args.hidden,
                             args.layers, args.actions, args.seed)
        print("artifact:", paths["artifact"])
        print("fixture :", paths["fixture"])
        return 0
    if args.command == "probe":
        paths = make_probe(args.artifact, args.out_dir, args.seq_len, args.seed)
        print("artifact:", paths["artifact"])
        print("fixture :", paths["fixture"])
        return 0
    if args.command == "check":
        with open(args.fixture) as handle:
            fixture = json.load(handle)
        with open(args.godot) as handle:
            produced = json.load(handle)
        problems = compare(fixture, produced, args.rtol, args.atol)
        if problems:
            print("MISMATCH (%d):" % len(problems))
            for problem in problems:
                print("  -", problem)
            return 1
        steps = sum(len(s["expected"]) for s in fixture["sequences"])
        print("OK: %d recurrent steps match the reference (rtol=%g atol=%g)"
              % (steps, args.rtol, args.atol))
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
