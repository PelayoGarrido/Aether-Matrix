#!/usr/bin/env python3
"""Reproducible fixed Pianist ONNX graphs using only the Python standard library.

Weights remain the pinned original BF16 safetensors file. External initializer
offsets address its verified data; Cast nodes expand to FP32 inside ONNX Runtime.
No PyTorch/ONNX Python package, pickle, network or duplicate FP32 checkpoint.
Architecture derived from Pianist Transformer / T5Gemma; Apache notices retained.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct

HERE = Path(__file__).resolve().parent
REVISION = "8f1568201d1ec035c73939d5a67c5168c0b80221"
SHA = "8fb9111efc147adcc61a4885bd42a6e6b625626d9fb8576811916927aa8281d0"
SIZE = 271506656
RESERVE = 3 * 1024**3


def varint(value):
    value &= (1 << 64) - 1
    out = bytearray()
    while value > 127:
        out.append((value & 127) | 128); value >>= 7
    out.append(value)
    return bytes(out)


def integer(field, value): return varint(field << 3) + varint(value)
def blob(field, value):
    if isinstance(value, str): value = value.encode()
    return varint((field << 3) | 2) + varint(len(value)) + value


def value_info(name, shape, dtype=1):
    dimensions = b"".join(blob(1, integer(1, d) if isinstance(d, int) else blob(2, d)) for d in shape)
    return blob(1, name) + blob(2, blob(1, integer(1, dtype) + blob(2, dimensions)))


def attribute(name, value):
    if isinstance(value, str): return blob(1, name) + blob(4, value) + integer(20, 3)
    if isinstance(value, int): return blob(1, name) + integer(3, value) + integer(20, 2)
    return blob(1, name) + b"".join(integer(8, v) for v in value) + integer(20, 7)


class Graph:
    def __init__(self, metadata):
        self.metadata = metadata
        self.nodes, self.initializers, self.inputs, self.outputs = [], {}, [], []
        self.serial = 0
        self.weight_casts = {}

    def op(self, kind, *inputs, name=None, **attrs):
        if name is None: name = "node_" + str(self.serial)
        self.serial += 1
        node = b"".join(blob(1, i) for i in inputs) + blob(2, name) + blob(3, name) + blob(4, kind)
        node += b"".join(blob(5, attribute(k, v)) for k, v in attrs.items())
        self.nodes.append(blob(1, node)); return name

    def constant(self, values, dtype=7, scalar=False):
        name = "constant_" + str(self.serial); self.serial += 1
        shape = [] if scalar else [len(values)]
        raw = struct.pack("<" + ("q" if dtype == 7 else "f") * len(values), *values)
        self.initializers[name] = b"".join(integer(1, d) for d in shape) + integer(2, dtype) + blob(8, name) + blob(9, raw)
        return name

    def weight(self, name):
        if name not in self.weight_casts:
            item = self.metadata["tensors"][name]
            if item["dtype"] != "BF16": raise ValueError("Unexpected checkpoint dtype")
            start, end = item["data_offsets"]
            if end - start != math.prod(item["shape"])*2: raise ValueError("Invalid checkpoint tensor extent")
            raw_name = name + ".bf16"
            tensor = b"".join(integer(1, d) for d in item["shape"]) + integer(2, 16) + blob(8, raw_name)
            for key, value in (("location", "model.safetensors"), ("offset", str(self.metadata["header_bytes"]+start)), ("length", str(end-start))):
                tensor += blob(13, blob(1, key)+blob(2, value))
            tensor += integer(14, 1)
            self.initializers[raw_name] = tensor
            self.weight_casts[name] = self.op("Cast", raw_name, to=1)
        return self.weight_casts[name]

    def linear(self, x, name, bias=False):
        weight = self.op("Transpose", self.weight(name+".weight"), perm=[1, 0])
        result = self.op("MatMul", x, weight)
        return self.op("Add", result, self.weight(name+".bias")) if bias else result

    def norm(self, x, name):
        squared = self.op("Mul", x, x)
        mean = self.op("ReduceMean", squared, self.constant([-1]), keepdims=1)
        inverse = self.op("Reciprocal", self.op("Sqrt", self.op("Add", mean, self.constant([1e-6], 1, True))))
        scaled = self.op("Mul", x, inverse)
        return self.op("Mul", scaled, self.op("Add", self.weight(name+".weight"), self.constant([1.0], 1, True)))

    def mlp(self, x, name):
        gate = self.op("Gelu", self.linear(x, name+".gate_proj"), approximate="tanh")
        return self.linear(self.op("Mul", gate, self.linear(x, name+".up_proj")), name+".down_proj")

    def heads(self, x, name, kind):
        projected = self.linear(x, name+"."+kind+"_proj")
        shaped = self.op("Reshape", projected, self.constant([1, -1, 8 if kind == "q" else 4, 128]))
        return self.op("Transpose", shaped, perm=[0, 2, 1, 3])

    def rotary(self, positions):
        positions = self.op("Cast", positions, to=1)
        positions = self.op("Unsqueeze", positions, self.constant([1]))
        angles = self.op("Mul", positions, self.constant([10000**(-i/128) for i in range(0, 128, 2)], 1))
        doubled = self.op("Concat", angles, angles, axis=1)
        doubled = self.op("Unsqueeze", doubled, self.constant([0, 1]))
        return self.op("Cos", doubled), self.op("Sin", doubled)

    def rotate(self, x, rotary):
        first = self.op("Slice", x, self.constant([0]), self.constant([64]), self.constant([3]))
        second = self.op("Slice", x, self.constant([64]), self.constant([128]), self.constant([3]))
        rotated = self.op("Concat", self.op("Neg", second), first, axis=3)
        return self.op("Add", self.op("Mul", x, rotary[0]), self.op("Mul", rotated, rotary[1]))

    def repeat_heads(self, x):
        length = self.op("Gather", self.op("Shape", x), self.constant([2]), axis=0)
        expanded = self.op("Unsqueeze", x, self.constant([2]))
        target = self.op("Concat", self.constant([1, 4, 2]), length, self.constant([128]), axis=0)
        expanded = self.op("Expand", expanded, target)
        return self.op("Reshape", expanded, self.constant([1, 8, -1, 128]))

    def attention(self, x, name, rotary=None, layer=None, cross=None):
        query = self.heads(x, name, "q")
        if cross is None:
            key, value = self.heads(x, name, "k"), self.heads(x, name, "v")
            query, key = self.rotate(query, rotary), self.rotate(key, rotary)
            if layer is not None:
                key = self.op("Concat", "past_key_"+str(layer), key, axis=2, name="present_key_"+str(layer))
                value = self.op("Concat", "past_value_"+str(layer), value, axis=2, name="present_value_"+str(layer))
        else: key, value = cross
        key, value = self.repeat_heads(key), self.repeat_heads(value)
        scores = self.op("MatMul", query, self.op("Transpose", key, perm=[0, 1, 3, 2]))
        scores = self.op("Mul", scores, self.constant([256**-0.5/50], 1, True))
        scores = self.op("Mul", self.op("Tanh", scores), self.constant([50.0], 1, True))
        output = self.op("MatMul", self.op("Softmax", scores, axis=-1), value)
        output = self.op("Transpose", output, perm=[0, 2, 1, 3])
        return self.linear(self.op("Reshape", output, self.constant([1, -1, 1024])), name+".o_proj")

    def model(self, name):
        graph = b"".join(self.nodes)+blob(2, name)+b"".join(blob(5, v) for v in self.initializers.values())
        graph += b"".join(blob(11, value_info(*v)) for v in self.inputs)
        graph += b"".join(blob(12, value_info(*v)) for v in self.outputs)
        return integer(1, 9)+blob(2, "AetherAudioLabs / Pianist Transformer adaptation")+blob(3, "pianist-native-1")+blob(7, graph)+blob(8, integer(2, 20))


def graphs(metadata):
    encoder = Graph(metadata); encoder.inputs = [("score", [1, "tokens"], 7)]
    x = encoder.op("Gather", encoder.weight("model.encoder.embeddings.word_embeddings.weight"), "score", axis=0)
    grouped = encoder.op("Reshape", x, encoder.constant([1, -1, 8, 768]))
    projected = [encoder.linear(encoder.op("Gather", grouped, encoder.constant([i], scalar=True), axis=2), f"model.encoder.embeddings.projection_layers.{i}", True) for i in range(8)]
    x = encoder.op("Mul", encoder.op("Concat", *projected, axis=2), encoder.constant([768**0.5], 1, True))
    length = encoder.op("Gather", encoder.op("Shape", x), encoder.constant([1], scalar=True), axis=0)
    positions = encoder.op("Range", encoder.constant([0], scalar=True), length, encoder.constant([1], scalar=True))
    rotary = encoder.rotary(positions)
    for i in range(10):
        p = f"model.encoder.layers.{i}"
        x = encoder.op("Add", x, encoder.norm(encoder.attention(encoder.norm(x, p+".pre_self_attn_layernorm"), p+".self_attn", rotary), p+".post_self_attn_layernorm"))
        x = encoder.op("Add", x, encoder.norm(encoder.mlp(encoder.norm(x, p+".pre_feedforward_layernorm"), p+".mlp"), p+".post_feedforward_layernorm"))
    hidden = encoder.op("Identity", encoder.norm(x, "model.encoder.norm"), name="hidden")
    encoder.outputs = [("hidden", [1, "notes", 768])]
    for i in range(2):
        for kind in ("k", "v"):
            name = "cross_"+kind+"_"+str(i)
            encoder.op("Identity", encoder.heads(hidden, f"model.decoder.layers.{i}.cross_attn", kind), name=name)
            encoder.outputs.append((name, [1, 4, "notes", 128]))
    decoder = Graph(metadata); decoder.inputs = [("token", [1, 1], 7), ("position", [1], 7)]
    for i in range(2):
        decoder.inputs += [("cross_"+kind+"_"+str(i), [1, 4, "notes", 128]) for kind in ("k", "v")]
        decoder.inputs += [("past_"+kind+"_"+str(i), [1, 4, "past", 128]) for kind in ("key", "value")]
    x = decoder.op("Gather", decoder.weight("model.decoder.embed_tokens.weight"), "token", axis=0)
    x = decoder.op("Mul", x, decoder.constant([768**0.5], 1, True)); rotary = decoder.rotary("position")
    for i in range(2):
        p = f"model.decoder.layers.{i}"
        x = decoder.op("Add", x, decoder.norm(decoder.attention(decoder.norm(x, p+".pre_self_attn_layernorm"), p+".self_attn", rotary, i), p+".post_self_attn_layernorm"))
        x = decoder.op("Add", x, decoder.norm(decoder.attention(decoder.norm(x, p+".pre_cross_attn_layernorm"), p+".cross_attn", cross=("cross_k_"+str(i), "cross_v_"+str(i))), p+".post_cross_attn_layernorm"))
        x = decoder.op("Add", x, decoder.norm(decoder.mlp(decoder.norm(x, p+".pre_feedforward_layernorm"), p+".mlp"), p+".post_feedforward_layernorm"))
    x = decoder.norm(x, "model.decoder.norm")
    logits = decoder.op("MatMul", x, decoder.op("Transpose", decoder.weight("model.decoder.embed_tokens.weight"), perm=[1, 0]))
    logits = decoder.op("Mul", decoder.op("Tanh", decoder.op("Div", logits, decoder.constant([30.0], 1, True))), decoder.constant([30.0], 1, True))
    decoder.op("Reshape", logits, decoder.constant([1, 5389]), name="logits")
    decoder.outputs = [("logits", [1, 5389])]+[("present_"+kind+"_"+str(i), [1, 4, "present", 128]) for i in range(2) for kind in ("key", "value")]
    return {"encoder.onnx": encoder.model("PianistEncoder"), "decoder.onnx": decoder.model("PianistCachedDecoder")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--graphs-only", action="store_true", help="No weights; useful for reproducibility/CI")
    args = parser.parse_args()
    metadata = json.loads((HERE/"checkpoint-tensors.json").read_text())
    data = graphs(metadata)
    source = HERE.parents[1]/".build-downloads/pianist-transformer/model.safetensors"
    if not args.graphs_only:
        if source.is_symlink() or source.stat().st_size != SIZE: raise ValueError("Pinned checkpoint missing or changed")
        with source.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != SHA: raise ValueError("Checkpoint integrity failed")
        with source.open("rb") as stream:
            size = struct.unpack("<Q", stream.read(8))[0]; actual = json.loads(stream.read(size)); actual.pop("__metadata__", None)
        if size+8 != metadata["header_bytes"] or actual != metadata["tensors"]: raise ValueError("Checkpoint tensor contract changed")
    # A same-volume link needs no second checkpoint. Cross-volume copying is
    # explicitly budgeted; graph-only CI needs only its small output budget.
    same_volume = not args.graphs_only and source.stat().st_dev == args.output.parent.stat().st_dev
    required = 4*1024**2 + (SIZE if not args.graphs_only and not same_volume else 0)
    if shutil.disk_usage(args.output.parent).free < RESERVE+required: raise ValueError("Preserve 3 GiB plus bounded export space")
    args.output.mkdir(mode=0o700)  # Exclusive: never replace an existing model/export.
    files = [{"path": name, "bytes": len(value), "sha256": hashlib.sha256(value).hexdigest()} for name, value in data.items()]
    for name, value in data.items():
        with (args.output/name).open("xb") as stream: stream.write(value)
    if not args.graphs_only:
        try: os.link(source, args.output/"model.safetensors")
        except OSError:
            if shutil.disk_usage(args.output).free < RESERVE+SIZE: raise ValueError("Checkpoint copy would cross the disk reserve")
            with source.open("rb") as src, (args.output/"model.safetensors").open("xb") as dst: shutil.copyfileobj(src, dst, 1024**2)
    files.append({"path": "model.safetensors", "bytes": SIZE, "sha256": SHA})
    manifest = dict(schema=1, exporter="pianist-native-1", source_revision=REVISION, files=files, weights_included=not args.graphs_only,
                    compute="FP32; original BF16 weights cast at session load", opset=20, redistribution_approved=False)
    (args.output/"manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__": main()
