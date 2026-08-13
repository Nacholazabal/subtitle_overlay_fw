#!/usr/bin/env python3

import hashlib
import io
import tarfile
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from server.evaluation.dataset_manifest import (
    DatasetValidationError,
    build_mediaspeech_manifest,
    manifest_fingerprint,
    normalize_spanish_text,
    safe_extract_archive,
    verify_archive,
)


class DatasetManifestTests(unittest.TestCase):
    def test_normalization_is_spanish_case_and_punctuation_insensitive(self):
        self.assertEqual("qué tal número 22", normalize_spanish_text("  ¿Qué TAL, número 22? "))

    def test_manifest_requires_exact_pairs_and_is_deterministic(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            for clip_id, text in (("b", "dos"), ("a", "uno")):
                (root / f"{clip_id}.flac").write_bytes(clip_id.encode())
                (root / f"{clip_id}.txt").write_text(text, encoding="utf-8")
            clips = build_mediaspeech_manifest(root, expected_clips=2)
            self.assertEqual(["a", "b"], [clip.clip_id for clip in clips])
            self.assertEqual("uno", clips[0].reference_normalized)
            self.assertEqual(manifest_fingerprint(clips), manifest_fingerprint(list(clips)))

    def test_manifest_rejects_a_missing_reference(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "orphan.flac").write_bytes(b"audio")
            with self.assertRaises(DatasetValidationError):
                build_mediaspeech_manifest(root, expected_clips=1)

    def test_archive_hash_can_be_verified(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "ES.tgz"
            path.write_bytes(b"fixed archive")
            expected = hashlib.sha256(b"fixed archive").hexdigest()
            self.assertEqual(expected, verify_archive(path, expected)["sha256"])

    def test_safe_extract_rejects_path_traversal(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "bad.tgz"
            with tarfile.open(archive, "w:gz") as bundle:
                info = tarfile.TarInfo("../escape.txt")
                payload = b"no"
                info.size = len(payload)
                bundle.addfile(info, io.BytesIO(payload))
            with self.assertRaises(DatasetValidationError):
                safe_extract_archive(archive, root / "out")


if __name__ == "__main__":
    unittest.main()
