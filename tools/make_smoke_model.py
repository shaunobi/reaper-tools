#!/usr/bin/env python3
"""Generates tests/data/smoke.onnx: y = 2*x + 1 over a float[1,4] input.

Used by the unit tests, srt_cli, and the "SRT: About" action to prove that
ONNX Runtime and the chosen execution provider work on this machine.
Requires the `onnx` package (developer side only).
"""
import pathlib
import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

x = helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 4])
y = helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 4])
two = numpy_helper.from_array(np.array([2.0], dtype=np.float32), name="two")
one = numpy_helper.from_array(np.array([1.0], dtype=np.float32), name="one")
mul = helper.make_node("Mul", ["x", "two"], ["xm"])
add = helper.make_node("Add", ["xm", "one"], ["y"])
graph = helper.make_graph([mul, add], "srt_smoke", [x], [y], initializer=[two, one])
model = helper.make_model(graph, producer_name="srt", opset_imports=[helper.make_opsetid("", 17)])
model.ir_version = 8
onnx.checker.check_model(model)
out = pathlib.Path(__file__).resolve().parent.parent / "tests" / "data" / "smoke.onnx"
out.parent.mkdir(parents=True, exist_ok=True)
onnx.save(model, str(out))
print(f"wrote {out} ({out.stat().st_size} bytes)")
