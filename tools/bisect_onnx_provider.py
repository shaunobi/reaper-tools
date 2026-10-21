#!/usr/bin/env python3
"""Finds the first node of an ONNX graph whose output differs between two
execution providers (default: CPU vs DirectML). Used to track down operators
that DirectML computes wrongly.

Runs the graph with batches of intermediate tensors exposed as extra outputs
and reports, in topological order, the first nodes whose outputs diverge.

Usage: bisect_onnx_provider.py <model.onnx> [--segment N] [--batch 60] [--tol 1e-2]
Inputs are random (seeded); for Demucs cores the mix is random audio and the
spectrogram is computed with the same host STFT as the extension.
Requires onnxruntime-directml (not onnxruntime) in the environment.
"""
import argparse
import math
import sys

import numpy as np
import onnx
import onnxruntime as ort


def host_spec(mix, nfft=4096, hop=1024):
    # HTDemucs._spec in numpy: reflect pad, normalised STFT, drop last bin, frames [2, 2+le).
    length = mix.shape[-1]
    le = int(math.ceil(length / hop))
    pad = hop // 2 * 3
    x = np.pad(mix, ((0, 0), (0, 0), (pad, pad + le * hop - length)), mode="reflect")
    # torch.stft center=True reflect
    x = np.pad(x, ((0, 0), (0, 0), (nfft // 2, nfft // 2)), mode="reflect")
    frames = 1 + (x.shape[-1] - nfft) // hop
    win = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(nfft) / nfft)
    idx = np.arange(frames)[:, None] * hop + np.arange(nfft)[None, :]
    z = np.fft.rfft(x[..., idx] * win, axis=-1) / math.sqrt(nfft)  # (B, C, frames, bins)
    z = z[..., :-1].transpose(0, 1, 3, 2)  # (B, C, bins, frames)
    z = z[..., 2:2 + le]
    return z.real.astype(np.float32).copy(), z.imag.astype(np.float32).copy()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--batch", type=int, default=60)
    ap.add_argument("--tol", type=float, default=1e-2)
    ap.add_argument("--providers", default="CPUExecutionProvider,DmlExecutionProvider")
    args = ap.parse_args()
    pa, pb = args.providers.split(",")

    model = onnx.load(args.model)
    graph = model.graph
    inputs = {i.name: i for i in graph.input}
    rng = np.random.default_rng(0)
    seg = [d.dim_value for d in inputs["mix"].type.tensor_type.shape.dim][-1]
    mix = (rng.standard_normal((1, 2, seg)) * 0.1).astype(np.float32)
    sr_, si_ = host_spec(mix)
    feeds = {"mix": mix, "spec_real": sr_, "spec_imag": si_}

    # Shape-only tensors are constant-folded away by the providers; exposing
    # them as graph outputs breaks session creation (and DirectML then falls
    # back to CPU, masking everything in the batch). Skip them up front.
    skip_ops = {"Shape", "Constant", "ConstantOfShape", "Range", "Gather", "Unsqueeze", "Squeeze", "Concat", "Cast",
                "Slice", "Reshape", "Transpose", "Expand", "Where", "Equal", "Identity"}
    names = []
    for node in graph.node:
        if node.op_type in skip_ops:
            continue
        for out in node.output:
            if out:
                names.append((node.op_type, node.name, out))
    print(f"{len(graph.node)} nodes, {len(names)} compute tensors to compare; {pa} vs {pb}")

    # Whole-graph outputs first.
    outs = [o.name for o in graph.output]
    sa = ort.InferenceSession(args.model, providers=[pa])
    sb = ort.InferenceSession(args.model, providers=[pb])
    ra = sa.run(outs, feeds)
    rb = sb.run(outs, feeds)
    for n, a, b in zip(outs, ra, rb):
        print(f"graph output {n}: max |a| {np.abs(a).max():.3g}, max |b| {np.abs(b).max():.3g}, max diff {np.abs(a - b).max():.3g}")

    existing = set(outs)
    base = onnx.load(args.model)
    state = {"reported": 0, "checked": 0}

    def sessions_for(chunk):
        m = onnx.ModelProto()
        m.CopyFrom(base)
        for _, _, out in chunk:
            m.graph.output.append(onnx.ValueInfoProto(name=out))
        data = m.SerializeToString()
        sa = ort.InferenceSession(data, providers=[pa])
        sb = ort.InferenceSession(data, providers=[pb])
        # The Python wrapper silently falls back to CPU when the provider rejects
        # the graph; that would compare CPU with CPU, so treat it as a failure.
        if pb not in sb.get_providers():
            raise RuntimeError("provider fell back")
        return sa, sb

    def compare(chunk):
        if not chunk:
            return
        try:
            sa, sb = sessions_for(chunk)
            want = [t[2] for t in chunk]
            ra = sa.run(want, feeds)
            rb = sb.run(want, feeds)
        except Exception as ex:
            if len(chunk) == 1:
                print(f"  cannot expose {chunk[0][0]} -> {chunk[0][2]} ({str(ex)[:80]}); skipped")
                return
            mid = len(chunk) // 2
            compare(chunk[:mid])
            compare(chunk[mid:])
            return
        for (op, nname, out), a, b in zip(chunk, ra, rb):
            state["checked"] += 1
            if a.dtype.kind not in "fc" or a.shape != b.shape:
                continue
            scale = max(np.abs(a).max(), 1e-6)
            diff = np.abs(a.astype(np.float64) - b.astype(np.float64)).max()
            if diff / scale > args.tol or not np.isfinite(b).all():
                print(f"DIVERGES: {op} '{nname}' -> {out} shape {a.shape}: max |cpu| {scale:.3g}, max |dml| {np.abs(b).max():.3g}, "
                      f"max diff {diff:.3g} (rel {diff / scale:.2e}), finite {np.isfinite(b).all()}", flush=True)
                state["reported"] += 1
                if state["reported"] >= 8:
                    raise SystemExit("(stopping after 8 divergent tensors; the first is the one to fix)")

    for start in range(0, len(names), args.batch):
        compare([t for t in names[start:start + args.batch] if t[2] not in existing])
        print(f"  checked {state['checked']} of {len(names)}", flush=True)
    print("no divergence found" if state["reported"] == 0 else f"{state['reported']} divergent tensors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
