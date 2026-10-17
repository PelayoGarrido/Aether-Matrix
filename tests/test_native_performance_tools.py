import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("native_export", ROOT/"tools/performance/export_native.py")
export = importlib.util.module_from_spec(spec); spec.loader.exec_module(export)


class NativeGraphTests(unittest.TestCase):
    def test_fixed_graphs_reproduce_pinned_bundled_architecture(self):
        metadata = json.loads((ROOT/"tools/performance/checkpoint-tensors.json").read_text())
        manifest = json.loads((ROOT/"tools/performance/native-manifest.json").read_text())
        generated = export.graphs(metadata)
        for name, data in generated.items():
            item = next(i for i in manifest["files"] if i["path"] == name)
            self.assertEqual(len(data), item["bytes"])
            self.assertEqual(hashlib.sha256(data).hexdigest(), item["sha256"])
            self.assertEqual(data, (ROOT/"models"/("pianist_"+name)).read_bytes())

    def test_every_external_tensor_is_bounded_and_nonoverlapping(self):
        metadata = json.loads((ROOT/"tools/performance/checkpoint-tensors.json").read_text())
        spans = []
        for tensor in metadata["tensors"].values():
            start, end = tensor["data_offsets"]
            self.assertEqual(tensor["dtype"], "BF16")
            self.assertGreater(end, start)
            self.assertGreaterEqual(start, 0)
            count = 1
            for dimension in tensor["shape"]:
                self.assertGreater(dimension, 0); count *= dimension
            self.assertEqual(end-start, count*2)
            self.assertLessEqual(metadata["header_bytes"]+end, export.SIZE)
            spans.append((start, end))
        spans.sort()
        self.assertEqual(spans[0][0], 0)
        for previous, current in zip(spans, spans[1:]): self.assertEqual(previous[1], current[0])
        self.assertEqual(spans[-1][1]+metadata["header_bytes"], export.SIZE)

    def test_export_refuses_to_overwrite_an_existing_job(self):
        with tempfile.TemporaryDirectory() as directory:
            job = Path(directory)/"existing"; job.mkdir()
            sentinel = job/"keep.txt"; sentinel.write_bytes(b"user data")
            result = subprocess.run([sys.executable, str(ROOT/"tools/performance/export_native.py"), str(job), "--graphs-only"], capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(sentinel.read_bytes(), b"user data")
            self.assertEqual(list(job.iterdir()), [sentinel])

    def test_incomplete_parity_report_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            job = Path(directory); fixture = job/"tokens.json"
            data = json.dumps(dict(input=[2]*16, output=[2]*16)).encode(); fixture.write_bytes(data)
            (job/"native.json").write_text(json.dumps(dict(completed=True, source_fixture_sha256=hashlib.sha256(data).hexdigest(), tokens=16, snapshots=[])))
            result = subprocess.run([sys.executable, str(ROOT/"tools/performance/compare_native.py"), str(fixture), str(job)], capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"evidence is incomplete", result.stderr)
            self.assertFalse((job/"parity.json").exists())


if __name__ == "__main__": unittest.main()
