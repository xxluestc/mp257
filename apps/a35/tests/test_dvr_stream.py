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

    def test_existing_partial_file_belongs_to_other_encoder(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "event.mp4"
            part = Path(f"{output}.part")
            part.write_bytes(b"another encoder owns this")
            with patch.object(worker, "require_storage"):
                self.assertEqual(worker.process(io.BytesIO(), output, Path(directory), "ffmpeg"), 2)
            self.assertEqual(part.read_bytes(), b"another encoder owns this")

    def test_cleanup_failure_preserves_original_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "event.mp4"
            with patch.object(worker, "require_storage"), \
                 patch.object(worker.shutil, "which", return_value=None), \
                 patch.object(Path, "unlink", side_effect=OSError("storage disappeared")):
                self.assertEqual(worker.process(io.BytesIO(), output, Path(directory), "ffmpeg"), 2)

    def test_committed_video_is_successful_when_rotation_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "event.mp4"

            def encoded(stream, part, log):
                part.write_bytes(b"validated video")

            with patch.object(worker, "require_storage"), \
                 patch.object(worker.shutil, "which", return_value="available"), \
                 patch.object(worker, "encode_ffmpeg", side_effect=encoded), \
                 patch.object(worker, "validate", return_value=True), \
                 patch.object(worker.os, "open", return_value=123), \
                 patch.object(worker.os, "O_DIRECTORY", 0, create=True), \
                 patch.object(worker.os, "fsync"), patch.object(worker.os, "close"), \
                 patch.object(worker, "prune_old_videos", side_effect=OSError("rotation failed")):
                self.assertEqual(worker.process(io.BytesIO(), output, root, "ffmpeg"), 0)
            self.assertEqual(output.read_bytes(), b"validated video")

    def test_mp4_trailing_partial_box_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "event.mp4"
            valid = (struct.pack(">I4s", 8, b"ftyp") + struct.pack(">I4s", 4080, b"mdat") +
                     bytes(4072) + struct.pack(">I4s", 8, b"moov"))
            output.write_bytes(valid)
            self.assertTrue(dvr_validation.has_mp4_boxes(output))
            output.write_bytes(valid + b"trailer")
            self.assertFalse(dvr_validation.has_mp4_boxes(output))

    def test_retention_keeps_current_video(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            current = root / "emergency_current.mp4"
            previous = root / "emergency_previous.mp4"
            current.write_bytes(b"current")
            previous.write_bytes(b"previous")
            with patch.object(dvr_validation, "MAX_DVR_BYTES", 1):
                dvr_validation.prune_old_videos(root, current)
            self.assertTrue(current.exists())
            self.assertFalse(previous.exists())

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
