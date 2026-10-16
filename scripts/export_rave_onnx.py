#!/usr/bin/env python3
"""Export the deterministic reference decoder. This is not a trained RAVE model.

The legacy filename is retained for the plugin's model lookup contract.
No ML framework, network access, checkpoints, or training data are required.
"""
import argparse
import hashlib
import math
from pathlib import Path
import struct


def varint(value):
    out = bytearray()
    while value > 127:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def integer(field, value):
    return varint(field << 3) + varint(value)


def blob(field, value):
    if isinstance(value, str):
        value = value.encode('utf-8')
    return varint((field << 3) | 2) + varint(len(value)) + value


def value_info(name, shape):
    dimensions = b''.join(blob(1, integer(1, d)) for d in shape)
    tensor = integer(1, 1) + blob(2, dimensions)
    return blob(1, name) + blob(2, blob(1, tensor))


def tensor(name, shape, data_type, data):
    return (b''.join(integer(1, d) for d in shape) + integer(2, data_type)
            + blob(8, name) + blob(9, data))


def node(op, inputs, output):
    return (b''.join(blob(1, name) for name in inputs)
            + blob(2, output) + blob(4, op))


def export_model():
    basis = [math.sin(2 * math.pi * (channel + 2) * sample / 2048) / 16
             for channel in range(16) for sample in range(2048)]
    graph = b''.join(blob(1, node(op, inputs, output)) for op, inputs, output in [
        ('Flatten', ['z'], 'flat'), ('MatMul', ['flat', 'basis'], 'sum'),
        ('Tanh', ['sum'], 'bounded'), ('Reshape', ['bounded', 'shape'], 'audio')])
    graph += blob(2, 'AetherReferenceDecoder')
    graph += blob(5, tensor('basis', [16, 2048], 1, struct.pack('<32768f', *basis)))
    graph += blob(5, tensor('shape', [3], 7, struct.pack('<3q', 1, 1, 2048)))
    graph += blob(11, value_info('z', [1, 16, 1]))
    graph += blob(12, value_info('audio', [1, 1, 2048]))
    return (integer(1, 8) + blob(2, 'AetherAudioLabs') + blob(3, '0.1.0')
            + blob(6, 'Deterministic sine-basis reference decoder; not trained RAVE; 48000 Hz.')
            + blob(7, graph) + blob(8, integer(2, 17)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'models/rave_decoder.onnx')
    args = parser.parse_args()
    model = export_model()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(model)
    print(f'Reference decoder: {len(model)} bytes; SHA256 {hashlib.sha256(model).hexdigest()}')
