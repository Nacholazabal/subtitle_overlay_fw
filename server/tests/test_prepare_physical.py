import sys
import tempfile
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from server.evaluation.dataset_manifest import DatasetClip
from server.evaluation.prepare_physical import (
    Candidate,
    build_candidates,
    build_candidates_from_dataset,
    flac_stream_info,
    select_representative,
    selection_fingerprint,
)


def clip(index: int) -> DatasetClip:
    return DatasetClip(
        clip_id=f"clip-{index}",
        audio_relpath=f"clip-{index}.flac",
        reference_relpath=f"clip-{index}.txt",
        audio_sha256=f"{index:064x}",
        reference_sha256=f"{index + 1:064x}",
        reference_raw="hola mundo",
        reference_normalized="hola mundo",
        reference_words=2,
    )


def write_minimal_flac(path: Path, *, samples: int, rate: int = 16000, channels: int = 1) -> None:
    stream_info = bytearray(34)
    packed = (
        (rate << 44)
        | ((channels - 1) << 41)
        | ((16 - 1) << 36)
        | samples
    )
    stream_info[10:18] = packed.to_bytes(8, "big")
    path.write_bytes(b"fLaC" + bytes([0x80, 0, 0, 34]) + stream_info)


class PhysicalSelectionTests(unittest.TestCase):
    def test_flac_streaminfo_is_read_without_audio_dependencies(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.flac"
            write_minimal_flac(path, samples=32000)
            info = flac_stream_info(path)
        self.assertEqual(16000, info["sample_rate_hz"])
        self.assertEqual(1, info["channels"])
        self.assertEqual(16, info["bits_per_sample"])
        self.assertEqual(2.0, info["duration_sec"])

    def test_dataset_only_candidates_use_flac_duration_and_human_reference(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "clip-1.flac"
            write_minimal_flac(source, samples=16000)
            candidates = build_candidates_from_dataset([clip(1)], root)
        self.assertEqual(1.0, candidates[0].duration_sec)
        self.assertEqual(2.0, candidates[0].speech_rate_wps)
        self.assertEqual("short__slow", candidates[0].stratum)

    def test_candidates_use_frozen_duration_and_speed_strata(self):
        clips = [clip(1), clip(2), clip(3)]
        rows = {
            "clip-1": {"status": "ok", "audio_duration_sec": 8, "speech_rate_reference_wps": 2},
            "clip-2": {"status": "ok", "audio_duration_sec": 12, "speech_rate_reference_wps": 2.5},
            "clip-3": {"status": "ok", "audio_duration_sec": 16, "speech_rate_reference_wps": 3.2},
        }
        candidates = build_candidates(clips, rows)
        self.assertEqual(
            ["short__slow", "medium__medium", "long__fast"],
            [candidate.stratum for candidate in candidates],
        )

    def test_selection_is_deterministic_and_reaches_target(self):
        candidates = [
            Candidate(clip(index), 10.0 + index % 3, 2.0 + (index % 4) * 0.3,
                      f"band-{index % 3}")
            for index in range(30)
        ]
        first = select_representative(candidates, 100.0)
        second = select_representative(list(reversed(candidates)), 100.0)
        self.assertEqual([row.clip.clip_id for row in first], [row.clip.clip_id for row in second])
        self.assertGreaterEqual(sum(row.duration_sec for row in first), 100.0)
        self.assertEqual(selection_fingerprint(first), selection_fingerprint(second))

    def test_selection_rejects_a_target_larger_than_corpus(self):
        candidates = [Candidate(clip(1), 5.0, 2.0, "one")]
        with self.assertRaises(ValueError):
            select_representative(candidates, 6.0)


if __name__ == "__main__":
    unittest.main()
