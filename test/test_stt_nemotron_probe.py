import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
from pathlib import Path
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.stt_nemotron_probe import (
    EXPECTED_AUDIO_NAMES,
    NemotronProbeConfig,
    build_official_streaming_command,
    discover_short_audios,
    parse_streaming_debug_transcripts,
    prepare_probe_inputs,
    select_drive_audio_dir,
)


def _write_wav(path: Path, seconds: float = 0.01) -> None:
    frames = int(16_000 * seconds)
    with wave.open(str(path), "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(16_000)
        audio.writeframes(struct.pack("<" + "h" * frames, *([0] * frames)))


class ConfigTests(unittest.TestCase):
    def test_default_is_spanish_balanced_lookahead(self):
        config = NemotronProbeConfig()
        self.assertEqual("es-ES", config.target_lang)
        self.assertEqual(320, config.latency_ms)
        self.assertEqual((56, 3), config.att_context_size)
        self.assertEqual([56, 3], config.as_dict()["att_context_size"])

    def test_all_official_latencies_are_mapped(self):
        expected = {
            80: (56, 0),
            160: (56, 1),
            320: (56, 3),
            560: (56, 6),
            1120: (56, 13),
        }
        for latency, context in expected.items():
            with self.subTest(latency=latency):
                self.assertEqual(context, NemotronProbeConfig(latency_ms=latency).att_context_size)

    def test_invalid_language_latency_and_decoder_are_rejected(self):
        with self.assertRaises(ValueError):
            NemotronProbeConfig(target_lang="es")
        with self.assertRaises(ValueError):
            NemotronProbeConfig(latency_ms=123)
        with self.assertRaises(ValueError):
            NemotronProbeConfig(decoder_type="ctc")


class DriveDiscoveryTests(unittest.TestCase):
    def test_discovers_exact_clips_in_playback_order_recursively(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            nested = root / "audios"
            nested.mkdir()
            for name in reversed(EXPECTED_AUDIO_NAMES):
                (nested / name).write_bytes(b"webm")

            found = discover_short_audios(root)

            self.assertEqual(list(EXPECTED_AUDIO_NAMES), [path.name for path in found])

    def test_missing_and_duplicate_clips_fail_loudly(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in EXPECTED_AUDIO_NAMES[:-1]:
                (root / name).write_bytes(b"webm")
            with self.assertRaises(FileNotFoundError):
                discover_short_audios(root)

            (root / EXPECTED_AUDIO_NAMES[-1]).write_bytes(b"webm")
            duplicate_dir = root / "duplicate"
            duplicate_dir.mkdir()
            (duplicate_dir / EXPECTED_AUDIO_NAMES[0]).write_bytes(b"webm")
            with self.assertRaises(RuntimeError):
                discover_short_audios(root)

    def test_candidate_selection_uses_first_complete_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            incomplete = root / "incomplete"
            complete = root / "complete"
            incomplete.mkdir()
            complete.mkdir()
            for name in EXPECTED_AUDIO_NAMES:
                (complete / name).write_bytes(b"webm")
            self.assertEqual(complete.resolve(), select_drive_audio_dir([incomplete, complete]))


class PackageResolutionTests(unittest.TestCase):
    def test_repo_scripts_wins_over_foreign_regular_package(self):
        repo_root = Path(__file__).resolve().parents[1]
        marker = repo_root / "scripts" / "__init__.py"
        self.assertTrue(marker.is_file())
        with tempfile.TemporaryDirectory() as temporary:
            foreign = Path(temporary) / "scripts"
            foreign.mkdir()
            (foreign / "__init__.py").write_text("ORIGIN = 'foreign'\n", encoding="utf-8")
            environment = os.environ.copy()
            environment["PYTHONPATH"] = os.pathsep.join((str(repo_root), temporary))
            process = subprocess.run(
                [sys.executable, "-c", "import scripts; print(scripts.__file__)"],
                capture_output=True,
                text=True,
                env=environment,
                check=True,
            )
        self.assertEqual(str(marker), process.stdout.strip())


class InputPreparationTests(unittest.TestCase):
    def test_converts_clips_and_labels_optional_txt_without_claiming_human_truth(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source_dir = root / "source"
            source_dir.mkdir()
            for name in EXPECTED_AUDIO_NAMES:
                (source_dir / name).write_bytes(b"webm")
            (source_dir / "desay-short.txt").write_text("texto aproximado", encoding="utf-8")

            def fake_run(command, **_kwargs):
                _write_wav(Path(command[-1]))
                return mock.Mock(returncode=0, stderr="")

            with mock.patch("scripts.stt_nemotron_probe.subprocess.run", side_effect=fake_run):
                records, manifest = prepare_probe_inputs(source_dir, root / "work")

            self.assertEqual(3, len(records))
            self.assertEqual("provided_txt", records[0]["reference_kind"])
            self.assertEqual("missing", records[1]["reference_kind"])
            self.assertNotIn("text", records[1])
            parsed = [json.loads(line) for line in manifest.read_text(encoding="utf-8").splitlines()]
            self.assertEqual("es-ES", parsed[0]["target_lang"])
            self.assertEqual("texto aproximado", parsed[0]["text"])


class OfficialCommandTests(unittest.TestCase):
    def test_command_uses_pinned_experiment_semantics(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = root / "examples/asr/asr_cache_aware_streaming"
            script.mkdir(parents=True)
            (script / "speech_to_text_cache_aware_streaming_infer.py").write_text("", encoding="utf-8")
            manifest = root / "manifest.jsonl"
            manifest.write_text("", encoding="utf-8")

            command = build_official_streaming_command(
                root, manifest, root / "out", NemotronProbeConfig()
            )

            self.assertIn("target_lang=es-ES", command)
            self.assertIn("att_context_size=[56,3]", command)
            self.assertIn("decoder_type=rnnt", command)
            self.assertIn("amp=true", command)

    def test_debug_parser_extracts_partials_and_finals(self):
        log = """
        INFO Streaming transcriptions: ['hola']
        INFO Streaming transcriptions: ['hola mundo']
        INFO Final streaming transcriptions: ['hola mundo.']
        """
        parsed = parse_streaming_debug_transcripts(log)
        self.assertEqual([['hola'], ['hola mundo']], parsed["partials"])
        self.assertEqual([['hola mundo.']], parsed["finals"])


if __name__ == "__main__":
    unittest.main()
