#!/usr/bin/env python3
"""Compare native FP32 encoder/logit snapshots with the reviewed Torch reference.

Developer evaluation only. The native application does not import this script
or require Python. Both passes use identical teacher-forced token prefixes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time
from evaluate import SIZE, SHA


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tokens", type=Path); parser.add_argument("native_job", type=Path)
    parser.add_argument("--weights", type=Path, default=Path(__file__).resolve().parents[2]/".build-downloads/pianist-transformer/model.safetensors")
    args = parser.parse_args()
    with args.tokens.open("rb") as stream: data = stream.read(1024**2+1)
    if len(data)>1024**2: raise ValueError("Oversized teacher fixture")
    source = json.loads(data)
    for key in ("input", "output"):
        tokens = source.get(key)
        if not isinstance(tokens, list) or not 16 <= len(tokens) <= 4096 or len(tokens)%8:
            raise ValueError("Expected 2..512 complete teacher token groups")
        if any(type(t) is not int or not 0 <= t < 5389 for t in tokens): raise ValueError("Invalid teacher token")
    if len(source["input"]) != len(source["output"]): raise ValueError("Teacher token counts differ")
    with (args.native_job/"native.json").open("rb") as stream: native_data = stream.read(65537)
    if len(native_data)>65536: raise ValueError("Oversized native metadata")
    native = json.loads(native_data)
    required = sorted({0, 1, 7, 15, 63, 255, 511, len(source["output"])-1} & set(range(len(source["output"]))))
    if native.get("snapshots") != required or native.get("tokens") != len(source["output"]):
        raise ValueError("Native parity evidence is incomplete")
    if native.get("completed") is not True or native["source_fixture_sha256"] != hashlib.sha256(data).hexdigest():
        raise ValueError("Native job/teacher fixture mismatch")
    if args.weights.is_symlink() or args.weights.stat().st_size != SIZE: raise ValueError("Pinned weights missing or changed")
    with args.weights.open("rb") as stream:
        if hashlib.file_digest(stream,"sha256").hexdigest()!=SHA: raise ValueError("Weights failed verification")
    import torch
    from model import Pianist
    torch.set_num_threads(2); torch.set_num_interop_threads(1)
    def tensor(path, count):
        with path.open("rb") as stream: raw = stream.read(count*4+1)
        if len(raw)!=count*4: raise ValueError("Invalid native float snapshot size")
        return torch.frombuffer(bytearray(raw),dtype=torch.float32)
    model=Pianist(str(args.weights));started=time.monotonic()
    with torch.inference_mode():
        encoded=model.encode(source["input"]).reshape(-1)
        expected=tensor(args.native_job/"encoder.f32",encoded.numel())
        encoder_error=float((encoded-expected).abs().max())
        snapshots=[];previous=2
        for i,token in enumerate(source["output"]):
            logits=model.decode([previous]);previous=token
            if i in native["snapshots"]:
                expected=tensor(args.native_job/f"logits-{i}.f32",5389)
                error=logits-expected
                probabilities=logits.softmax(-1);other=expected.softmax(-1)
                snapshots.append(dict(step=i,max_abs_error=float(error.abs().max()),rms_error=float(error.square().mean().sqrt()),
                                      distribution_l1=float((probabilities-other).abs().sum())))
    report=dict(schema=1,encoder_max_abs_error=encoder_error,logits=snapshots,teacher_prefixes_identical=True,
                dtype="float32",source_weights_sha256=SHA,torch_version=torch.__version__,reference_seconds=time.monotonic()-started,
                passed=encoder_error<0.005 and all(s["max_abs_error"]<0.005 and s["distribution_l1"]<0.001 for s in snapshots),
                limitations="Measured float32, no-padding contexts and selected teacher prefixes; not musical acceptance or bit-exact backend equivalence")
    target=args.native_job/"parity.json"
    with target.open("x") as stream:json.dump(report,stream,indent=2,allow_nan=False);stream.write("\n")
    print(json.dumps(report,indent=2))
    if not report["passed"]:raise ValueError("Native/reference parity failed")


if __name__=="__main__":main()
