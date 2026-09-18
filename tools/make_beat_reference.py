#!/usr/bin/env python3
"""Builds the reference data for the beat-tracker correctness tests.

Writes into tests/data/beat/:
  clip.wav        synthetic 10 s drum-like clip, 22050 Hz mono, 16-bit
  spect.f32       log-mel spectrogram from beat_this.preprocessing.LogMelSpect (frames x 128, float32)
  beat.f32        beat logits from the ONNX model driven by beat_this's chunking (float32)
  downbeat.f32    downbeat logits (float32)
  beats.txt       beat times from beat_this's "minimal" postprocessor, one per line
  downbeats.txt   downbeat times, one per line
  meta.json       frames, tempo and the true hit times of the synthetic clip

Requires: torch, torchaudio, beat_this, onnxruntime, numpy (developer side only).
Usage: make_beat_reference.py <beat-this-final0.onnx>
"""
import json
import pathlib
import sys
import wave

import numpy as np
import onnxruntime as ort
import torch

from beat_this.inference import split_predict_aggregate
from beat_this.model.postprocessor import Postprocessor
from beat_this.preprocessing import LogMelSpect

SR = 22050
BPM = 121.3
DURATION = 10.0
OFFSET = 0.35  # first downbeat


def synth_clip(rng):
    n = int(DURATION * SR)
    y = np.zeros(n, dtype=np.float64)
    t = np.arange(n) / SR
    beat_len = 60.0 / BPM
    hits = []
    k = 0
    while True:
        time = OFFSET + k * beat_len
        if time >= DURATION - 0.1:
            break
        i = int(round(time * SR))
        beat_in_bar = k % 4
        # kick: decaying 60 Hz sine with pitch drop on every beat, louder on downbeat
        dur = int(0.25 * SR)
        tt = np.arange(dur) / SR
        env = np.exp(-tt * 18.0)
        kick = np.sin(2 * np.pi * (55.0 + 60.0 * np.exp(-tt * 40.0)) * tt) * env
        y[i:i + dur] += kick[: n - i] * (1.0 if beat_in_bar == 0 else 0.7)
        # snare-like noise burst on beats 2 and 4
        if beat_in_bar in (1, 3):
            dur = int(0.18 * SR)
            burst = rng.standard_normal(dur) * np.exp(-np.arange(dur) / SR * 30.0)
            y[i:i + dur] += 0.5 * burst[: n - i]
        # crash-like long bright noise on the downbeat
        if beat_in_bar == 0:
            dur = int(0.9 * SR)
            burst = rng.standard_normal(dur) * np.exp(-np.arange(dur) / SR * 4.0)
            y[i:i + dur] += 0.25 * burst[: n - i]
        # hi-hat eighths
        for sub in (0, 1):
            j = int(round((time + sub * beat_len / 2) * SR))
            dur = int(0.05 * SR)
            if j + dur < n:
                burst = rng.standard_normal(dur) * np.exp(-np.arange(dur) / SR * 90.0)
                y[j:j + dur] += 0.18 * burst
        hits.append({"time": time, "beat_in_bar": beat_in_bar})
        k += 1
    y /= np.max(np.abs(y)) * 1.05
    return y.astype(np.float32), hits


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    model_path = sys.argv[1]
    out = pathlib.Path(__file__).resolve().parent.parent / "tests" / "data" / "beat"
    out.mkdir(parents=True, exist_ok=True)

    rng = np.random.default_rng(1234)
    clip, hits = synth_clip(rng)
    pcm = np.clip(np.round(clip * 32767.0), -32768, 32767).astype(np.int16)
    with wave.open(str(out / "clip.wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    # Use exactly what the WAV holds, so C++ and Python see identical samples.
    signal = pcm.astype(np.float32) / 32768.0

    spect = LogMelSpect()(torch.tensor(signal, dtype=torch.float32))  # (frames, 128)
    spect.numpy().astype(np.float32).tofile(out / "spect.f32")

    sess = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])

    def model(chunk):  # chunk: (1, frames, 128) torch tensor
        b, d = sess.run(["beat", "downbeat"], {"spectrogram": chunk.numpy().astype(np.float32)})
        return {"beat": torch.from_numpy(b), "downbeat": torch.from_numpy(d)}

    with torch.inference_mode():
        pred = split_predict_aggregate(spect=spect, chunk_size=1500, border_size=6, overlap_mode="keep_first", model=model)
    beat, downbeat = pred["beat"].float(), pred["downbeat"].float()
    beat.numpy().astype(np.float32).tofile(out / "beat.f32")
    downbeat.numpy().astype(np.float32).tofile(out / "downbeat.f32")

    beats, downbeats = Postprocessor(type="minimal")(beat, downbeat)
    (out / "beats.txt").write_text("".join(f"{t:.6f}\n" for t in beats))
    (out / "downbeats.txt").write_text("".join(f"{t:.6f}\n" for t in downbeats))

    meta = {
        "sample_rate": SR,
        "frames": int(spect.shape[0]),
        "n_mels": int(spect.shape[1]),
        "fps": 50,
        "true_bpm": BPM,
        "true_first_downbeat": OFFSET,
        "true_hits": hits,
        "model_sha256": "e5e37b7d1802895e42559c5a1ced7619b1601d257a5c8e167c9d45de37c1b078",
    }
    (out / "meta.json").write_text(json.dumps(meta, indent=1))
    print(f"frames={spect.shape[0]} beats={len(beats)} downbeats={len(downbeats)}")
    print("beats:", np.round(beats, 3))
    print("downbeats:", np.round(downbeats, 3))
    return 0


if __name__ == "__main__":
    sys.exit(main())
