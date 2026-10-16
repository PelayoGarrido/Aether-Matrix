"""Network-free adversarial tests for candidate integrity and SMF validation."""
import hashlib
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "midi"))
import download_candidate as download
from evaluate import inspect_midi


def smf(body):
    return bytes.fromhex("4d546864000000060000000101e04d54726b") + struct.pack(">I", len(body)) + body


class MidiValidation(unittest.TestCase):
    def parse(self, body):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / "test.mid"
            target.write_bytes(smf(body))
            return inspect_midi(target)

    def test_standard_note_and_metadata(self):
        body = bytes.fromhex("00ff510307a12000ff5902000000c00000903c508360803c0000ff2f00")
        notes, metadata = self.parse(body)
        self.assertEqual(notes, [{"tick": 0, "duration": 480, "pitch": 60, "velocity": 80}])
        self.assertEqual(metadata["tempo_us"], 500000)
        self.assertEqual(metadata["end_tick"], 480)

    def test_rejects_hanging_notes(self):
        with self.assertRaisesRegex(ValueError, "hanging"):
            self.parse(bytes.fromhex("00903c5000ff2f00"))

    def test_rejects_ambiguous_overlap(self):
        with self.assertRaisesRegex(ValueError, "overlap"):
            self.parse(bytes.fromhex("00903c5000903c5000ff2f00"))

    def test_rejects_unmatched_note_off(self):
        with self.assertRaisesRegex(ValueError, "Unmatched"):
            self.parse(bytes.fromhex("00803c0000ff2f00"))

    def test_rejects_other_channels(self):
        with self.assertRaisesRegex(ValueError, "channel"):
            self.parse(bytes.fromhex("00913c5000ff2f00"))

    def test_rejects_unbounded_delta(self):
        with self.assertRaisesRegex(ValueError, "delta"):
            self.parse(bytes.fromhex("ffffffff00ff2f00"))

    def test_rejects_missing_end(self):
        with self.assertRaisesRegex(ValueError, "end-of-track"):
            self.parse(bytes.fromhex("00c000"))

    def test_rejects_truncated_events(self):
        for body in (b"\x81", b"\x00", b"\x00\xff", b"\x00\xc0", b"\x00\x90\x3c"):
            with self.subTest(body=body), self.assertRaises(ValueError):
                self.parse(body)


class CandidateIntegrity(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)
        self.payload = b"verified fixed model payload"
        self.files = (("test.onnx", len(self.payload), hashlib.sha256(self.payload).hexdigest()),)
        self.root_patch = patch.object(download, "ROOT", self.root)
        self.files_patch = patch.object(download, "FILES", self.files)
        self.root_patch.start(); self.files_patch.start()
        self.addCleanup(self.root_patch.stop); self.addCleanup(self.files_patch.stop)

    def test_corrupt_existing_file_is_preserved_and_rejected(self):
        target = self.root / "test.onnx"
        target.write_bytes(b"x" * len(self.payload))
        with patch.object(download.urllib.request, "urlopen") as network:
            with self.assertRaisesRegex(RuntimeError, "integrity"):
                download.main()
            network.assert_not_called()
        self.assertEqual(target.read_bytes(), b"x" * len(self.payload))

    def test_low_storage_does_not_start_download(self):
        with patch.object(download.shutil, "disk_usage", return_value=type("Usage", (), {"free": download.RESERVE})()):
            with patch.object(download.urllib.request, "urlopen") as network:
                with self.assertRaisesRegex(RuntimeError, "storage"):
                    download.main()
                network.assert_not_called()

    def test_symlink_is_not_followed(self):
        original = self.root / "original"
        original.write_bytes(self.payload)
        (self.root / "test.onnx").symlink_to(original)
        with self.assertRaisesRegex(RuntimeError, "symlinks"):
            download.main()
        self.assertEqual(original.read_bytes(), self.payload)

    def response(self, payload, status=200, content_range=""):
        response = io.BytesIO(payload)
        response.status = status
        response.url = "https://huggingface.co/fixed"
        response.headers = {"Content-Length": str(len(payload)), "Content-Range": content_range}
        return response

    def test_download_is_hash_verified_and_atomically_promoted(self):
        with patch.object(download, "ensure_space"), patch.object(download.urllib.request, "urlopen", return_value=self.response(self.payload)):
            download.main()
        self.assertEqual((self.root / "test.onnx").read_bytes(), self.payload)
        self.assertFalse((self.root / "test.onnx.part").exists())

    def test_wrong_hash_is_never_promoted(self):
        with patch.object(download, "ensure_space"), patch.object(download.urllib.request, "urlopen", return_value=self.response(b"x" * len(self.payload))):
            with self.assertRaisesRegex(RuntimeError, "SHA256"):
                download.main()
        self.assertFalse((self.root / "test.onnx").exists())

    def test_server_ignoring_range_does_not_append_duplicate_weights(self):
        partial = self.root / "test.onnx.part"
        partial.write_bytes(self.payload[:5])
        with patch.object(download, "ensure_space"), patch.object(download.urllib.request, "urlopen", return_value=self.response(self.payload)):
            with self.assertRaisesRegex(RuntimeError, "partial download"):
                download.main()
        self.assertEqual(partial.read_bytes(), self.payload[:5])


if __name__ == "__main__":
    unittest.main()
