#!/usr/bin/env python3
import hashlib
from pathlib import Path
from export_rave_onnx import export_model
root = Path(__file__).resolve().parents[1]
data = (root / 'models/rave_decoder.onnx').read_bytes()
if data != export_model():
    raise SystemExit('Checked-in model differs from the reproducible reference exporter')
print('Reference model matches exporter:', hashlib.sha256(data).hexdigest())
