#!/usr/bin/env python3
"""Reference stems for the Demucs correctness test.

Runs the official demucs package (htdemucs_ft via demucs.apply.apply_model,
shifts=0, overlap=0.25, split=True) on tests/data/sep/mix.wav and writes the
[1.5 s, 2.0 s) slice of each stem to tests/data/sep/demucs/<stem>.f32
(stereo interleaved float32) plus meta.json. The C++ DemucsSeparator must
reproduce these from the ONNX exports.

The 4 s mix is shorter than one 7.8 s segment, so this exercises the centred
zero padding of a short chunk. The overlap-add across segments is covered by
a C++ unit test with an identity model.

Usage: .venv\\Scripts\\python tools\\make_demucs_reference.py
"""
import json
import pathlib
import sys
import wave

import numpy as np
import torch

from demucs.apply import apply_model
from demucs.pretrained import get_model

STEMS = ["drums", "bass", "other", "vocals"]
SLICE = (1.5, 2.0)


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    src = root / "tests" / "data" / "sep" / "mix.wav"
    out = root / "tests" / "data" / "sep" / "demucs"
    out.mkdir(parents=True, exist_ok=True)
    with wave.open(str(src), "rb") as w:
        assert w.getnchannels() == 2 and w.getsampwidth() == 2
        sr = w.getframerate()
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2)
    mix = torch.from_numpy((pcm.astype(np.float32) / 32768.0).T.copy())[None]  # (1, 2, n)

    bag = get_model("htdemucs_ft")
    bag.eval()
    torch.manual_seed(0)
    with torch.no_grad():
        stems = apply_model(bag, mix, shifts=0, split=True, overlap=0.25, progress=False, device="cpu")[0].numpy()
    assert stems.shape[0] == 4 and list(bag.sources) == STEMS

    a, b = int(SLICE[0] * sr), int(SLICE[1] * sr)
    for s, name in enumerate(STEMS):
        stems[s, :, a:b].T.astype(np.float32).tofile(out / f"{name}.f32")
    meta = {"sample_rate": sr, "slice_start": a, "slice_end": b, "stems": STEMS, "shifts": 0, "overlap": 0.25,
            "segment_samples": int(bag.models[0].segment * bag.samplerate)}
    (out / "meta.json").write_text(json.dumps(meta, indent=1))
    rms = lambda x: float(np.sqrt(np.mean(np.square(x))))
    for s, name in enumerate(STEMS):
        print(f"  {name:6s} rms {rms(stems[s]):.4f}")
    print("sum-null rms", rms(mix[0].numpy() - stems.sum(0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
