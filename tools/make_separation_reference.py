#!/usr/bin/env python3
"""Builds the reference data for the stem separation correctness test.

Writes into tests/data/sep/:
  mix.wav          synthetic 4 s stereo "song" (drums + bass + pad + lead), 44.1 kHz, 16-bit
  drums_part.f32   the drum part alone (stereo interleaved float32), for a sanity check
  <stem>.f32       0.5 s slice [1.5 s, 2.0 s) of each separated stem, stereo interleaved float32
  meta.json        slice offsets, stem order, model checksum

The separation runs the ONNX model through the same host STFT/iSTFT and
chunked overlap-add that silverdaw's reference script uses, so the C++
implementation can be compared to it. With a 4 s clip exactly one chunk is
processed. Requires numpy and onnxruntime (developer side only).
Usage: make_separation_reference.py <bs_roformer_4stem_rhythm_fp16.onnx>
"""
import hashlib
import json
import pathlib
import sys
import wave

import numpy as np
import onnxruntime as ort

SR = 44100
N_FFT = 2048
HOP = 441
PAD = N_FFT // 2
BINS = N_FFT // 2 + 1
HANN = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(N_FFT) / N_FFT)).astype(np.float64)
STEMS = ["drums", "bass", "other", "vocals"]
DURATION = 4.0
SLICE = (1.5, 2.0)


def synth(rng):
    n = int(DURATION * SR)
    t = np.arange(n) / SR
    bpm = 118.0
    beat = 60.0 / bpm
    drums = np.zeros((2, n))
    bass = np.zeros((2, n))
    pad = np.zeros((2, n))
    lead = np.zeros((2, n))
    k = 0
    while k * beat < DURATION:
        i = int(k * beat * SR)
        # kick every beat, snare on 2 and 4, hats on eighths
        d = int(0.3 * SR)
        tt = np.arange(d) / SR
        kick = np.sin(2 * np.pi * (50 + 70 * np.exp(-tt * 35)) * tt) * np.exp(-tt * 14)
        drums[:, i:i + d] += kick[: n - i] * 0.9
        if k % 2 == 1:
            d2 = int(0.2 * SR)
            sn = rng.standard_normal(d2) * np.exp(-np.arange(d2) / SR * 28) * 0.5
            drums[0, i:i + d2] += sn[: n - i]
            drums[1, i:i + d2] += sn[: n - i] * 0.8
        for sub in (0, 1):
            j = int((k * beat + sub * beat / 2) * SR)
            d3 = int(0.06 * SR)
            if j + d3 < n:
                hh = rng.standard_normal(d3) * np.exp(-np.arange(d3) / SR * 80) * 0.15
                drums[0, j:j + d3] += hh * 0.7
                drums[1, j:j + d3] += hh
        k += 1
    # bass: root notes changing every bar, sawtooth-ish
    notes = [55.0, 55.0, 41.2, 49.0]
    for bar in range(int(DURATION / (4 * beat)) + 1):
        a = int(bar * 4 * beat * SR)
        b = min(n, int((bar + 1) * 4 * beat * SR))
        f = notes[bar % 4]
        tt = t[a:b] - t[a] if b > a else t[a:b]
        env = np.minimum(1.0, tt * 50) * np.exp(-tt * 0.8)
        wave_ = sum(np.sin(2 * np.pi * f * h * tt) / h for h in (1, 2, 3, 4)) * 0.25
        bass[:, a:b] += wave_ * env
    # pad: slow chord, stereo detuned
    for f in (220.0, 277.2, 329.6):
        pad[0] += 0.06 * np.sin(2 * np.pi * f * t)
        pad[1] += 0.06 * np.sin(2 * np.pi * f * 1.003 * t + 0.5)
    # lead: vibrato tone with formant-like harmonics
    f0 = 440.0 * (1 + 0.01 * np.sin(2 * np.pi * 5.5 * t))
    phase = 2 * np.pi * np.cumsum(f0) / SR
    lead_m = sum(np.sin(h * phase) * w for h, w in ((1, 0.5), (2, 0.35), (3, 0.2), (4, 0.1)))
    lead_m *= 0.2 * (0.5 + 0.5 * np.sin(2 * np.pi * 0.5 * t)) ** 2
    lead[0] += lead_m * 0.9
    lead[1] += lead_m
    mix = drums + bass + pad + lead
    peak = np.max(np.abs(mix))
    mix /= peak * 1.1
    drums /= peak * 1.1
    return mix.astype(np.float32), drums.astype(np.float32)


def reflect_idx(idx, n):
    idx = np.where(idx < 0, -idx, idx)
    idx = np.where(idx >= n, 2 * n - 2 - idx, idx)
    return idx


def stft_chunk(chunk, T):
    real = np.zeros((1, 2, BINS, T), np.float32)
    imag = np.zeros((1, 2, BINS, T), np.float32)
    n = chunk.shape[1]
    base = (np.arange(T)[:, None] * HOP + np.arange(N_FFT)[None, :]) - PAD
    idx = reflect_idx(base, n)
    for ch in range(2):
        spec = np.fft.rfft(chunk[ch][idx] * HANN[None, :], axis=1)
        real[0, ch] = spec.real.T.astype(np.float32)
        imag[0, ch] = spec.imag.T.astype(np.float32)
    return real, imag


def istft_chunk(real, imag, T, chunk_len):
    out = np.zeros((2, chunk_len), np.float32)
    padded = (T - 1) * HOP + N_FFT
    for ch in range(2):
        time = np.fft.irfft((real[ch] + 1j * imag[ch]).T, n=N_FFT, axis=1)
        acc = np.zeros(padded, np.float64)
        env = np.zeros(padded, np.float64)
        for f in range(T):
            b = f * HOP
            acc[b:b + N_FFT] += time[f] * HANN
            env[b:b + N_FFT] += HANN * HANN
        out[ch] = (acc / np.maximum(env, 1e-8))[PAD:PAD + chunk_len].astype(np.float32)
    return out


def separate(sess, mix, chunk_len, T):
    samples = mix.shape[1]
    step = chunk_len // 2
    win = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(chunk_len) / chunk_len)
    acc = np.zeros((4, 2, samples + chunk_len), np.float64)
    cnt = np.zeros(samples + chunk_len, np.float64)
    chunk = np.zeros((2, chunk_len), np.float32)
    off = 0
    while off < samples:
        clen = min(chunk_len, samples - off)
        chunk[:] = 0.0
        chunk[:, :clen] = mix[:, off:off + clen]
        sr_, si_ = stft_chunk(chunk, T)
        or_, oi_ = sess.run(["out_spec_real", "out_spec_imag"], {"spec_real": sr_, "spec_imag": si_})
        for s in range(4):
            rec = istft_chunk(or_[0, s], oi_[0, s], T, chunk_len)
            for ch in range(2):
                acc[s, ch, off:off + chunk_len] += rec[ch] * win
        cnt[off:off + chunk_len] += win
        off += step
    cnt = np.maximum(cnt, 1e-8)
    return (acc / cnt[None, None, :])[:, :, :samples].astype(np.float32)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    model = sys.argv[1]
    out = pathlib.Path(__file__).resolve().parent.parent / "tests" / "data" / "sep"
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(42)
    mix, drums = synth(rng)
    pcm = np.clip(np.round(mix.T * 32767.0), -32768, 32767).astype(np.int16)
    with wave.open(str(out / "mix.wav"), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    mix_q = (pcm.astype(np.float32) / 32768.0).T  # exactly what the WAV holds
    a, b = int(SLICE[0] * SR), int(SLICE[1] * SR)
    drums[:, a:b].T.astype(np.float32).tofile(out / "drums_part.f32")

    sess = ort.InferenceSession(model, providers=["CPUExecutionProvider"])
    T = sess.get_inputs()[0].shape[3]
    chunk_len = (T - 1) * HOP
    stems = separate(sess, mix_q, chunk_len, T)
    for s, name in enumerate(STEMS):
        stems[s, :, a:b].T.astype(np.float32).tofile(out / f"{name}.f32")
    sha = hashlib.sha256(open(model, "rb").read()).hexdigest()
    meta = {"sample_rate": SR, "slice_start": a, "slice_end": b, "stems": STEMS, "chunk_samples": chunk_len,
            "frames": T, "model_sha256": sha}
    (out / "meta.json").write_text(json.dumps(meta, indent=1))
    rms = lambda x: float(np.sqrt(np.mean(np.square(x))))
    print("chunk", chunk_len, "frames", T)
    for s, name in enumerate(STEMS):
        print(f"  {name:6s} rms {rms(stems[s]):.4f}")
    print("sum-null rms", rms(mix_q - stems.sum(0)))
    d = stems[0, :, a:b]
    print("drums stem vs drum part corr", float(np.corrcoef(d.ravel(), drums[:, a:b].ravel())[0, 1]))
    print("mix vs drum part corr", float(np.corrcoef(mix_q[:, a:b].ravel(), drums[:, a:b].ravel())[0, 1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
