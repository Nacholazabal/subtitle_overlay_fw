#!/usr/bin/env python3
"""Lightweight helpers for the Nemotron 3.5 / NeMo Colab probe.

The real NeMo and torch imports intentionally live in the Colab notebook.  This
module only owns deterministic configuration, Drive audio discovery, conversion
to the 16 kHz mono WAV contract, manifest creation and parsing of the official
NeMo cache-aware streaming script output.  Keeping those pieces dependency-free
lets the WSL test suite validate the experiment without installing a GPU stack.
"""

from __future__ import annotations

import ast
import json
import re
import subprocess
import sys
import wave
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


RUN_ENGINE = "nemotron_3_5_nemo"
MODEL_ID = "nvidia/nemotron-3.5-asr-streaming-0.6b"
TARGET_SAMPLE_RATE = 16_000
EXPECTED_AUDIO_NAMES = (
    "desay-short.webm",
    "noticiero-short.webm",
    "rel-short.webm",
)

# Official model-card mapping. Each encoder frame represents 80 ms and the
# second context value is the number of right-context frames.
LATENCY_TO_ATT_CONTEXT = {
    80: (56, 0),
    160: (56, 1),
    320: (56, 3),
    560: (56, 6),
    1120: (56, 13),
}


@dataclass(frozen=True)
class NemotronProbeConfig:
    """Configuration shared by the offline and cache-aware probe cells."""

    model_id: str = MODEL_ID
    target_lang: str = "es-ES"
    latency_ms: int = 320
    decoder_type: str = "rnnt"
    strip_lang_tags: bool = True

    def __post_init__(self) -> None:
        if not self.model_id.strip():
            raise ValueError("model_id must not be empty")
        if not re.fullmatch(r"[a-z]{2}-[A-Z]{2}|auto", self.target_lang):
            raise ValueError("target_lang must be a locale such as es-ES or auto")
        if self.latency_ms not in LATENCY_TO_ATT_CONTEXT:
            supported = ", ".join(str(value) for value in LATENCY_TO_ATT_CONTEXT)
            raise ValueError(f"unsupported latency_ms={self.latency_ms}; choose {supported}")
        if self.decoder_type != "rnnt":
            raise ValueError("Nemotron 3.5 probe currently requires decoder_type='rnnt'")

    @property
    def att_context_size(self) -> tuple[int, int]:
        return LATENCY_TO_ATT_CONTEXT[self.latency_ms]

    def as_dict(self) -> dict:
        data = asdict(self)
        data.update(
            {
                "run_engine": RUN_ENGINE,
                "att_context_size": list(self.att_context_size),
                "target_sample_rate_hz": TARGET_SAMPLE_RATE,
            }
        )
        return data


def discover_short_audios(audio_dir: Path) -> list[Path]:
    """Return the three thesis clips in their fixed playback order."""
    audio_dir = Path(audio_dir).expanduser().resolve()
    if not audio_dir.is_dir():
        raise FileNotFoundError(f"audio directory does not exist: {audio_dir}")

    duplicates: dict[str, list[Path]] = {}
    for name in EXPECTED_AUDIO_NAMES:
        matches = list(audio_dir.rglob(name))
        if len(matches) > 1:
            duplicates[name] = matches
    if duplicates:
        detail = "; ".join(f"{name}: {paths}" for name, paths in duplicates.items())
        raise RuntimeError(f"duplicate short-audio files in Drive: {detail}")

    by_name = {
        path.name: path
        for name in EXPECTED_AUDIO_NAMES
        for path in audio_dir.rglob(name)
    }
    missing = [name for name in EXPECTED_AUDIO_NAMES if name not in by_name]
    if missing:
        raise FileNotFoundError(
            f"missing {missing} below {audio_dir}; expected exactly {EXPECTED_AUDIO_NAMES}"
        )
    return [by_name[name] for name in EXPECTED_AUDIO_NAMES]


def select_drive_audio_dir(candidates: Iterable[Path]) -> Path:
    """Select the first candidate containing all clips, with useful diagnostics."""
    checked: list[str] = []
    for candidate in candidates:
        candidate = Path(candidate)
        checked.append(str(candidate))
        try:
            discover_short_audios(candidate)
        except (FileNotFoundError, RuntimeError):
            continue
        return candidate.resolve()
    raise FileNotFoundError(
        "could not find the three short audios in any Drive candidate:\n  - "
        + "\n  - ".join(checked)
    )


def _reference_for(source: Path) -> tuple[str, str]:
    reference_path = source.with_suffix(".txt")
    if not reference_path.is_file():
        return "", "missing"
    return reference_path.read_text(encoding="utf-8").strip(), "provided_txt"


def convert_to_wav(source: Path, destination: Path, *, ffmpeg: str = "ffmpeg") -> None:
    """Convert one media clip to NeMo's mono PCM WAV input contract."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    process = subprocess.run(
        [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(source),
            "-ac",
            "1",
            "-ar",
            str(TARGET_SAMPLE_RATE),
            "-c:a",
            "pcm_s16le",
            str(destination),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if process.returncode != 0:
        raise RuntimeError(
            f"ffmpeg failed for {source.name}: {process.stderr.strip()[:500]}"
        )


def wav_duration_sec(path: Path) -> float:
    with wave.open(str(path), "rb") as audio:
        if audio.getnchannels() != 1 or audio.getframerate() != TARGET_SAMPLE_RATE:
            raise ValueError(f"unexpected WAV format after conversion: {path}")
        return round(audio.getnframes() / float(audio.getframerate()), 3)


def prepare_probe_inputs(
    audio_dir: Path,
    work_dir: Path,
    *,
    ffmpeg: str = "ffmpeg",
) -> tuple[list[dict], Path]:
    """Convert clips and write the NeMo JSONL manifest used by both probes."""
    work_dir = Path(work_dir).resolve()
    wav_dir = work_dir / "audio-16k"
    records: list[dict] = []
    for source in discover_short_audios(audio_dir):
        wav_path = wav_dir / f"{source.stem}.wav"
        convert_to_wav(source, wav_path, ffmpeg=ffmpeg)
        reference, reference_kind = _reference_for(source)
        record = {
            "audio_filepath": str(wav_path),
            "source_audio_filepath": str(source),
            "reference_kind": reference_kind,
            "duration": wav_duration_sec(wav_path),
            "target_lang": "es-ES",
        }
        # NeMo interprets every present ``text`` field as ground truth and tries
        # to calculate WER. An empty placeholder would therefore manufacture a
        # meaningless score (or divide by zero), so omit it until a .txt exists.
        if reference_kind != "missing":
            record["text"] = reference
        records.append(record)

    manifest = work_dir / "nemotron-probe-manifest.jsonl"
    manifest.parent.mkdir(parents=True, exist_ok=True)
    with manifest.open("w", encoding="utf-8") as handle:
        for record in records:
            handle.write(json.dumps(record, ensure_ascii=False) + "\n")
    return records, manifest


def build_offline_transcribe_config(model, config: NemotronProbeConfig):
    """Build the prompt-safe config for complete-file NeMo transcription.

    The prompt-aware RNNT model's current Lhotse path writes temporary entries
    without a supervision language when ``audio`` is a plain list of paths. The
    prompt dataset then tries to resolve the literal key ``None``. The non-Lhotse
    path intentionally leaves prompt indices out of the batch, which makes the
    model use ``trcfg.target_lang`` in its documented dynamic-prompt fallback.
    """
    transcribe_config = model.get_transcribe_config()
    transcribe_config.use_lhotse = False
    transcribe_config.batch_size = 1
    transcribe_config.return_hypotheses = True
    transcribe_config.num_workers = 0
    transcribe_config.verbose = False
    transcribe_config.target_lang = config.target_lang
    return transcribe_config


def build_official_streaming_command(
    nemo_root: Path,
    manifest: Path,
    output_dir: Path,
    config: NemotronProbeConfig,
) -> list[str]:
    """Build the official NeMo cache-aware simulation command."""
    script = (
        Path(nemo_root)
        / "examples"
        / "asr"
        / "asr_cache_aware_streaming"
        / "speech_to_text_cache_aware_streaming_infer.py"
    )
    if not script.is_file():
        raise FileNotFoundError(f"official NeMo streaming script not found: {script}")
    left, right = config.att_context_size
    return [
        sys.executable,
        str(script),
        f"pretrained_name={config.model_id}",
        f"dataset_manifest={Path(manifest).resolve()}",
        "batch_size=1",
        f"target_lang={config.target_lang}",
        f"att_context_size=[{left},{right}]",
        f"decoder_type={config.decoder_type}",
        f"strip_lang_tags={str(config.strip_lang_tags).lower()}",
        f"output_path={Path(output_dir).resolve()}",
        "compare_vs_offline=false",
        "amp=true",
        "compute_dtype=float32",
        "debug_mode=true",
        "cuda=0",
    ]


_STREAMING_LINE = re.compile(r"Streaming transcriptions:\s*(\[.*\])")
_FINAL_LINE = re.compile(r"Final streaming transcriptions:\s*(\[.*\])")


def _parse_transcription_lists(pattern: re.Pattern[str], log_text: str) -> list[list[str]]:
    parsed: list[list[str]] = []
    for match in pattern.finditer(log_text):
        try:
            value = ast.literal_eval(match.group(1))
        except (SyntaxError, ValueError):
            continue
        if isinstance(value, list) and all(isinstance(item, str) for item in value):
            parsed.append(value)
    return parsed


def parse_streaming_debug_transcripts(log_text: str) -> dict:
    """Extract incremental and final hypotheses from NeMo's official debug log."""
    return {
        "partials": _parse_transcription_lists(_STREAMING_LINE, log_text),
        "finals": _parse_transcription_lists(_FINAL_LINE, log_text),
    }


def write_json(path: Path, payload: object) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)
