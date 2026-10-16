#!/usr/bin/env python3
"""Exports Demucs v4 fine-tuned (htdemucs_ft) to ONNX with the STFT outside the graph.

htdemucs_ft is a bag of four HTDemucs models, one specialised per stem
(drums, bass, other, vocals). Each is exported as its own ONNX file holding
the network between the spectrogram and the inverse spectrogram:

  inputs   mix        float[1, 2, 343980]      the raw (un-normalised) 7.8 s segment
           spec_real  float[1, 2, 2048, 336]   host STFT of the segment (see below)
           spec_imag  float[1, 2, 2048, 336]
  outputs  out_spec_real float[1, 4, 2, 2048, 336]  per-stem spectrogram, de-normalised
           out_spec_imag float[1, 4, 2, 2048, 336]
           out_time      float[1, 4, 2, 343980]     per-stem time-branch output, de-normalised

The host must then run HTDemucs._ispec on the spectrogram outputs and add
out_time (srt_core/src/demucs.cpp does this). Normalisation of both branches
stays inside the graph because it only needs the inputs.

The host STFT is HTDemucs._spec: reflect-pad by (1536, 1536 + 336*1024 - 343980),
torch.stft(n_fft 4096, hop 1024, periodic Hann, normalized=True, center=True,
reflect), drop the last frequency bin, keep frames [2, 338).

Also writes <dest>/htdemucs_ft.json with sizes and SHA-256 for the model
registry, and checks each export against PyTorch on a random segment plus
the complete pipeline (wrapper + host-side inverse) against the original
model's forward.

Usage: .venv\\Scripts\\python tools\\export_demucs.py <dest-dir>
Requires torch, demucs, onnx, onnxruntime (see docs/BUILDING.md).
"""
import hashlib
import json
import math
import pathlib
import sys

import numpy as np
import torch
import torch.nn.functional as F
from torch import nn

from demucs.pretrained import get_model
from demucs.apply import BagOfModels
from demucs.htdemucs import HTDemucs

STEMS = ["drums", "bass", "other", "vocals"]


class Core(nn.Module):
    """HTDemucs.forward from after _spec/_magnitude to before _mask/_ispec."""

    def __init__(self, m: HTDemucs):
        super().__init__()
        self.m = m

    def forward(self, mix, spec_real, spec_imag):
        m = self.m
        B = mix.shape[0]
        # cac layout: (B, C*2, F, T) with re/im interleaved per channel, as _magnitude builds it.
        x = torch.stack([spec_real, spec_imag], dim=2)
        _, C, _, Fq, T = x.shape
        x = x.reshape(B, C * 2, Fq, T)

        mean = x.mean(dim=(1, 2, 3), keepdim=True)
        std = x.std(dim=(1, 2, 3), keepdim=True)
        x = (x - mean) / (1e-5 + std)

        xt = mix
        meant = xt.mean(dim=(1, 2), keepdim=True)
        stdt = xt.std(dim=(1, 2), keepdim=True)
        xt = (xt - meant) / (1e-5 + stdt)

        saved = []
        saved_t = []
        lengths = []
        lengths_t = []
        for idx, encode in enumerate(m.encoder):
            lengths.append(x.shape[-1])
            inject = None
            if idx < len(m.tencoder):
                lengths_t.append(xt.shape[-1])
                tenc = m.tencoder[idx]
                xt = tenc(xt)
                if not tenc.empty:
                    saved_t.append(xt)
                else:
                    inject = xt
            x = encode(x, inject)
            if idx == 0 and m.freq_emb is not None:
                frs = torch.arange(x.shape[-2], device=x.device)
                emb = m.freq_emb(frs).t()[None, :, :, None].expand_as(x)
                x = x + m.freq_emb_scale * emb
            saved.append(x)
        if m.crosstransformer:
            if m.bottom_channels:
                b, c, f, t = x.shape
                x = x.reshape(b, c, f * t)
                x = m.channel_upsampler(x)
                x = x.reshape(b, -1, f, t)
                xt = m.channel_upsampler_t(xt)
            x, xt = m.crosstransformer(x, xt)
            if m.bottom_channels:
                b, c, f, t = x.shape
                x = x.reshape(b, c, f * t)
                x = m.channel_downsampler(x)
                x = x.reshape(b, -1, f, t)
                xt = m.channel_downsampler_t(xt)
        for idx, decode in enumerate(m.decoder):
            skip = saved.pop(-1)
            x, pre = decode(x, skip, lengths.pop(-1))
            offset = m.depth - len(m.tdecoder)
            if idx >= offset:
                tdec = m.tdecoder[idx - offset]
                length_t = lengths_t.pop(-1)
                if tdec.empty:
                    pre = pre[:, :, 0]
                    xt, _ = tdec(pre, None, length_t)
                else:
                    skip = saved_t.pop(-1)
                    xt, _ = tdec(xt, skip, length_t)

        S = len(m.sources)
        x = x.view(B, S, -1, Fq, T)
        x = x * std[:, None] + mean[:, None]
        out = x.view(B, S, C, 2, Fq, T)
        out_real = out[:, :, :, 0]
        out_imag = out[:, :, :, 1]
        xt = xt.view(B, S, -1, mix.shape[-1])
        xt = xt * stdt[:, None] + meant[:, None]
        return out_real.contiguous(), out_imag.contiguous(), xt.contiguous()


def host_spec(m: HTDemucs, mix):
    """Exactly HTDemucs._spec; returns real and imaginary parts."""
    z = m._spec(mix)
    return z.real.contiguous(), z.imag.contiguous()


def host_pipeline(m: HTDemucs, core: Core, mix):
    """Wrapper plus the host-side inverse: must equal m(mix)."""
    sr_, si_ = host_spec(m, mix)
    out_r, out_i, xt = core(mix, sr_, si_)
    z = torch.complex(out_r, out_i)
    x = m._ispec(z, mix.shape[-1])
    return x + xt


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    dest = pathlib.Path(sys.argv[1])
    dest.mkdir(parents=True, exist_ok=True)
    torch.manual_seed(0)
    # nn.MultiheadAttention takes a fused "fast path" in eval mode that the
    # ONNX exporter cannot translate (aten::_native_multi_head_attention).
    torch.backends.mha.set_fastpath_enabled(False)

    # F.glu exports as Split -> Sigmoid -> Mul. ONNX Runtime's DirectML
    # provider computes that wrongly when a 1x1 Conv feeds the Split (the
    # outputs come out in the hundreds; verified with a three-node graph,
    # independent of the optimisation level), and every HTDemucs encoder,
    # decoder and DConv layer has exactly that pattern. Two Slices instead of
    # the Split are exact on DirectML, so export the gate that way.
    def glu_by_slicing(x, dim=-1):
        half = x.shape[dim] // 2
        a = x.narrow(dim, 0, half)
        g = x.narrow(dim, half, half)
        return a * torch.sigmoid(g)

    F.glu = glu_by_slicing
    torch.nn.functional.glu = glu_by_slicing

    bag = get_model("htdemucs_ft")
    assert isinstance(bag, BagOfModels), type(bag)
    print("bag of", len(bag.models), "models, sources", bag.sources, "weights", bag.weights)
    assert list(bag.sources) == STEMS

    manifest = {"name": "htdemucs_ft", "sources": STEMS, "models": []}
    for idx, (m, weights) in enumerate(zip(bag.models, bag.weights)):
        assert isinstance(m, HTDemucs)
        stem = STEMS[int(np.argmax(weights))]
        assert sum(weights) == max(weights) == 1.0, weights  # one stem per model
        m.eval()
        m.use_train_segment = True
        seg = int(m.segment * m.samplerate)
        hop = m.hop_length
        le = int(math.ceil(seg / hop))
        print(f"model {idx}: stem {stem}, segment {float(m.segment)} s = {seg} samples, nfft {m.nfft}, hop {hop}, "
              f"frames {le}, bins {m.nfft // 2}, cac {m.cac}, bottom_channels {m.bottom_channels}, depth {m.depth}")
        assert m.cac and m.samplerate == 44100 and m.nfft == 4096 and hop == 1024

        core = Core(m).eval()
        mix = torch.randn(1, 2, seg) * 0.1
        with torch.no_grad():
            ref = m(mix)
            ours = host_pipeline(m, core, mix)
            diff = (ref - ours).abs().max().item()
            print(f"  wrapper + host inverse vs original forward: max abs diff {diff:.2e}")
            assert diff < 1e-4, diff

            sr_, si_ = host_spec(m, mix)
            path = dest / f"htdemucs_ft_{stem}.onnx"
            torch.onnx.export(
                core, (mix, sr_, si_), str(path), export_params=True, opset_version=17, do_constant_folding=True,
                input_names=["mix", "spec_real", "spec_imag"],
                output_names=["out_spec_real", "out_spec_imag", "out_time"], dynamo=False)

            import onnxruntime as ort
            sess = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
            got = sess.run(None, {"mix": mix.numpy(), "spec_real": sr_.numpy(), "spec_imag": si_.numpy()})
            exp = core(mix, sr_, si_)
            for name, g, e in zip(["out_spec_real", "out_spec_imag", "out_time"], got, exp):
                d = np.abs(g - e.numpy()).max()
                print(f"  onnx vs torch {name}: max abs diff {d:.2e} (max |ref| {np.abs(e.numpy()).max():.2f})")
                assert d < 2e-3, (name, d)

        manifest["models"].append({
            "stem": stem, "file": path.name, "bytes": path.stat().st_size, "sha256": sha256(path),
            "segment_samples": seg, "nfft": m.nfft, "hop": hop, "frames": le, "bins": m.nfft // 2,
        })
        print(f"  wrote {path.name}: {path.stat().st_size / 1e6:.1f} MB")

    (dest / "htdemucs_ft.json").write_text(json.dumps(manifest, indent=1))
    print("manifest written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
