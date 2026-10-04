import io
from pathlib import Path
import struct
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import dvr_encode_worker as worker
import dvr_validation

JPEG = b"\xff\xd8payload\xff\xd9"


def record(timestamp, jpeg=JPEG):
    return struct.pack("<QI", timestamp, len(jpeg)) + jpeg


class FragmentedReader(io.BytesIO):
    def read(self, size=-1):
        return super().read(min(size, 3))


class StreamTests(unittest.TestCase):
    def test_fragmented_pipe_and_end_marker(self):
        data = record(1) + record(40001) + bytes(12)
        self.assertEqual(list(worker.records(FragmentedReader(data))), [(1, JPEG), (40001, JPEG)])

    def test_eof_cannot_commit_partial_event(self):
        for data in (b"", record(1), record(1) + record(2), record(1)[:-1]):
            with self.subTest(data=data), self.assertRaises(ValueError):
                list(worker.records(io.BytesIO(data)))

    def test_invalid_records(self):
        examples = (struct.pack("<QI", 1, worker.MAX_JPEG_BYTES + 1),
                    record(1) + record(1) + bytes(12),
                    record(2) + record(1) + bytes(12),
                    record(1, b"oops") + bytes(12), record(1) + bytes(12),
                    record(1) + record(worker.MAX_STREAM_US + 2))
        for data in examples:
            with self.subTest(data=data), self.assertRaises(ValueError):
                list(worker.records(io.BytesIO(data)))

    def test_uvc_padding_after_eoi(self):
        jpeg = JPEG + bytes(128)
        self.assertEqual(list(worker.records(io.BytesIO(record(1, jpeg) + record(2, jpeg) + bytes(12))))[0][1], jpeg)

    def test_encoder_failure_leaves_no_final_video(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "event.mp4"
            part = Path(str(output) + ".part")

            def interrupted(stream, path, log):
                path.write_bytes(b"partial")
                list(worker.records(stream))

            with patch.object(worker, "require_storage"), \
                 patch.object(worker.shutil, "which", return_value="available"), \
                 patch.object(worker, "encode_ffmpeg", side_effect=interrupted):
                result = worker.process(io.BytesIO(record(1)), output, Path(directory), "ffmpeg")
            self.assertEqual(result, 2)
            self.assertFalse(output.exists())
            self.assertFalse(part.exists())

    def test_unmounted_storage_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(RuntimeError):
                worker.require_storage(root, root / "event.mp4")

    @unittest.skipUnless(shutil.which("ffmpeg"), "native FFmpeg is unavailable")
    def test_real_stream_encoder_and_full_decode(self):
        # Exercise the actual image2pipe command and subprocess lifecycle.
        fixture = subprocess.run([
            "ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=128x96:rate=25",
            "-frames:v", "50", "-c:v", "mjpeg", "-f", "image2pipe", "pipe:1",
        ], check=True, capture_output=True, timeout=30).stdout
        frames = [block + b"\xff\xd9" for block in fixture.split(b"\xff\xd9")[:-1]]
        self.assertEqual(len(frames), 50)
        data = b"".join(record(1 + i * 40000, jpeg) for i, jpeg in enumerate(frames)) + bytes(12)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            part = root / "video.mp4.part"
            worker.encode_ffmpeg(io.BytesIO(data), part, root / "encode.log")
            self.assertTrue(dvr_validation.has_mp4_boxes(part))
            subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", str(part), "-f", "null", "-"],
                           check=True, capture_output=True, timeout=30)
            if shutil.which("ffprobe"):
                self.assertTrue(dvr_validation.validate(part, root / "probe.log", root / "decode.log"))


if __name__ == "__main__":
    unittest.main()
