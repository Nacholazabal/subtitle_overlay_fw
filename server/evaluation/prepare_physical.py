#!/usr/bin/env python3
"""Build the immutable one-hour MediaSpeech bundle for physical replay.

This is phase 3 of the fixed Nemotron evaluation.  It does not load NeMo or run
inference.  It can derive duration/speech-rate metadata directly from the
labelled corpus (the normal local path), or verify and reuse a completed frozen
evaluation when one is supplied.  In both modes it selects the same
deterministic representative subset and concatenates the original FLAC clips
with controlled silence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from server.evaluation.dataset_manifest import (
    ARCHIVE_SHA256,
    DATASET_ID,
    DATASET_VERSION,
    DatasetClip,
    build_mediaspeech_manifest,
    manifest_fingerprint,
    read_manifest,
    sha256_file,
)
from server.evaluation.dataset import EVALUATION_ID, fixed_nemotron_config


SCHEMA_VERSION = 1
BUNDLE_ID = "mediaspeech-es-v1.1__nemotron-560-600-2__physical-v1"
SELECTION_SEED = "subtitle-overlay-fw-physical-v1"
TARGET_SPEECH_SEC = 3600.0
LEADING_SILENCE_SEC = 6.0
INTER_CLIP_SILENCE_SEC = 1.2
TRAILING_SILENCE_SEC = 6.0


@dataclass(frozen=True)
class Candidate:
    clip: DatasetClip
    duration_sec: float
    speech_rate_wps: float
    stratum: str


def _read_json(path: Path) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _latest_ok_jsonl(path: Path) -> dict[str, dict]:
    latest: dict[str, dict] = {}
    with Path(path).open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            if not line.strip():
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"invalid JSON at {path}:{line_number}") from exc
            clip_id = row.get("clip_id")
            if isinstance(clip_id, str):
                latest[clip_id] = row
    return {key: value for key, value in latest.items() if value.get("status") == "ok"}


def _duration_band(value: float) -> str:
    return "short" if value < 10.0 else "medium" if value < 14.0 else "long"


def _speed_band(value: float) -> str:
    return "slow" if value < 2.3 else "medium" if value < 2.9 else "fast"


def build_candidates(clips: Sequence[DatasetClip], streaming_rows: dict[str, dict]) -> list[Candidate]:
    candidates: list[Candidate] = []
    for clip in clips:
        row = streaming_rows.get(clip.clip_id)
        if row is None:
            raise ValueError(f"missing successful frozen streaming row for {clip.clip_id}")
        duration = float(row["audio_duration_sec"])
        speed = float(row["speech_rate_reference_wps"])
        if not math.isfinite(duration) or duration <= 0 or not math.isfinite(speed):
            raise ValueError(f"invalid frozen metadata for {clip.clip_id}")
        candidates.append(
            Candidate(
                clip=clip,
                duration_sec=duration,
                speech_rate_wps=speed,
                stratum=f"{_duration_band(duration)}__{_speed_band(speed)}",
            )
        )
    return candidates


def flac_stream_info(path: Path) -> dict:
    """Read sample rate/channels/total samples from the mandatory FLAC STREAMINFO block."""
    path = Path(path)
    with path.open("rb") as handle:
        if handle.read(4) != b"fLaC":
            raise ValueError(f"not a native FLAC file: {path}")
        header = handle.read(4)
        if len(header) != 4 or (header[0] & 0x7F) != 0:
            raise ValueError(f"FLAC STREAMINFO is not the first metadata block: {path}")
        block_size = int.from_bytes(header[1:4], "big")
        stream_info = handle.read(block_size)
    if block_size != 34 or len(stream_info) != 34:
        raise ValueError(f"invalid FLAC STREAMINFO size in {path}: {block_size}")
    packed = int.from_bytes(stream_info[10:18], "big")
    sample_rate = (packed >> 44) & 0xFFFFF
    channels = ((packed >> 41) & 0x7) + 1
    bits_per_sample = ((packed >> 36) & 0x1F) + 1
    total_samples = packed & ((1 << 36) - 1)
    if sample_rate <= 0 or total_samples <= 0:
        raise ValueError(f"invalid FLAC duration metadata in {path}")
    return {
        "sample_rate_hz": sample_rate,
        "channels": channels,
        "bits_per_sample": bits_per_sample,
        "total_samples": total_samples,
        "duration_sec": total_samples / float(sample_rate),
    }


def build_candidates_from_dataset(
    clips: Sequence[DatasetClip], dataset_root: Path
) -> list[Candidate]:
    """Derive selection metadata directly from the labelled corpus, without model results."""
    candidates: list[Candidate] = []
    for clip in clips:
        info = flac_stream_info(clip.audio_path(dataset_root))
        if info["sample_rate_hz"] != 16000 or info["channels"] != 1:
            raise ValueError(
                f"expected 16 kHz mono MediaSpeech audio: {clip.audio_path(dataset_root)}"
            )
        duration = float(info["duration_sec"])
        speed = clip.reference_words / duration
        candidates.append(
            Candidate(
                clip=clip,
                duration_sec=duration,
                speech_rate_wps=speed,
                stratum=f"{_duration_band(duration)}__{_speed_band(speed)}",
            )
        )
    return candidates


def _stable_rank(clip_id: str, *, salt: str = "select") -> str:
    return hashlib.sha256(f"{SELECTION_SEED}\0{salt}\0{clip_id}".encode("utf-8")).hexdigest()


def select_representative(
    candidates: Sequence[Candidate], target_speech_sec: float = TARGET_SPEECH_SEC
) -> list[Candidate]:
    """Duration-proportional selection within fixed duration/speed strata."""
    if target_speech_sec <= 0:
        raise ValueError("target_speech_sec must be positive")
    total_duration = sum(candidate.duration_sec for candidate in candidates)
    if total_duration < target_speech_sec:
        raise ValueError(
            f"corpus has only {total_duration:.1f}s, below target {target_speech_sec:.1f}s"
        )

    by_stratum: dict[str, list[Candidate]] = {}
    for candidate in candidates:
        by_stratum.setdefault(candidate.stratum, []).append(candidate)

    selected: list[Candidate] = []
    for stratum in sorted(by_stratum):
        rows = by_stratum[stratum]
        stratum_duration = sum(row.duration_sec for row in rows)
        target = target_speech_sec * stratum_duration / total_duration
        accumulated = 0.0
        for row in sorted(rows, key=lambda item: _stable_rank(item.clip.clip_id)):
            if accumulated >= target:
                break
            selected.append(row)
            accumulated += row.duration_sec

    # Playback order is independent from selection order and interleaves strata.
    return sorted(selected, key=lambda item: _stable_rank(item.clip.clip_id, salt="playback"))


def selection_fingerprint(selected: Iterable[Candidate]) -> str:
    digest = hashlib.sha256()
    for candidate in selected:
        digest.update(candidate.clip.clip_id.encode("utf-8"))
        digest.update(b"\0")
        digest.update(candidate.clip.audio_sha256.encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def validate_frozen_evaluation(evaluation_dir: Path, clips: Sequence[DatasetClip]) -> dict:
    evaluation = _read_json(evaluation_dir / "evaluation.json")
    summary = _read_json(evaluation_dir / "summary.json")
    identity = summary.get("identity") or evaluation.get("identity") or evaluation
    if summary.get("status") != "complete":
        raise ValueError("frozen dataset evaluation is not complete")
    if identity.get("evaluation_id") != EVALUATION_ID:
        raise ValueError(f"unexpected evaluation_id: {identity.get('evaluation_id')!r}")
    if identity.get("dataset_archive_sha256") != ARCHIVE_SHA256:
        raise ValueError("frozen evaluation archive hash does not match MediaSpeech ES v1.1")
    if identity.get("dataset_manifest_sha256") != manifest_fingerprint(clips):
        raise ValueError("manifest fingerprint does not match the frozen evaluation")
    expected_config = fixed_nemotron_config().as_effective_config()
    if identity.get("config") != expected_config:
        raise ValueError("frozen evaluation is not the selected Nemotron 560/600/2 configuration")
    return {
        "evaluation_fingerprint": evaluation.get("evaluation_fingerprint")
        or evaluation.get("fingerprint")
        or identity.get("evaluation_fingerprint"),
        "identity": identity,
        "summary_sha256": sha256_file(evaluation_dir / "summary.json"),
        "streaming_results_sha256": sha256_file(evaluation_dir / "streaming_results.jsonl"),
    }


def _write_json(path: Path, payload: dict) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    temporary.replace(path)


def _run_ffmpeg(arguments: Sequence[str], *, purpose: str) -> None:
    completed = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip()[-4000:]
        raise RuntimeError(f"ffmpeg failed while {purpose}: {detail}")


def _concat_file_line(path: Path) -> str:
    escaped = path.resolve().as_posix().replace("'", "'\\''")
    return f"file '{escaped}'\n"


def _render_replay_audio(
    selected: Sequence[Candidate], dataset_root: Path, audio_path: Path
) -> tuple[list[dict], int]:
    """Concatenate source FLACs and exact silence with one ffmpeg re-encode."""
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("ffmpeg is required to build the local physical replay")

    cues: list[dict] = []
    cursor_samples = round(LEADING_SILENCE_SEC * 16000)
    with tempfile.TemporaryDirectory(prefix=".physical-build-", dir=audio_path.parent) as temporary:
        work = Path(temporary)
        silence_paths = {
            "leading": work / "leading.flac",
            "gap": work / "gap.flac",
            "trailing": work / "trailing.flac",
        }
        for name, seconds in (
            ("leading", LEADING_SILENCE_SEC),
            ("gap", INTER_CLIP_SILENCE_SEC),
            ("trailing", TRAILING_SILENCE_SEC),
        ):
            _run_ffmpeg(
                [
                    "-f",
                    "lavfi",
                    "-i",
                    "anullsrc=r=16000:cl=mono",
                    "-t",
                    f"{seconds:.6f}",
                    "-c:a",
                    "flac",
                    str(silence_paths[name]),
                ],
                purpose=f"creating {name} silence",
            )

        concat_lines = [_concat_file_line(silence_paths["leading"])]
        for index, candidate in enumerate(selected):
            source_path = candidate.clip.audio_path(dataset_root)
            if sha256_file(source_path) != candidate.clip.audio_sha256:
                raise ValueError(f"source audio hash mismatch: {source_path}")
            info = flac_stream_info(source_path)
            if info["sample_rate_hz"] != 16000 or info["channels"] != 1:
                raise ValueError(f"expected 16 kHz mono FLAC: {source_path}")
            start_sample = cursor_samples
            cursor_samples += int(info["total_samples"])
            end_sample = cursor_samples
            cues.append(
                {
                    "index": index,
                    "clip_id": candidate.clip.clip_id,
                    "stratum": candidate.stratum,
                    "source_audio_relpath": candidate.clip.audio_relpath,
                    "source_audio_sha256": candidate.clip.audio_sha256,
                    "reference_raw": candidate.clip.reference_raw,
                    "reference_normalized": candidate.clip.reference_normalized,
                    "reference_words": candidate.clip.reference_words,
                    "speech_rate_reference_wps": round(candidate.speech_rate_wps, 6),
                    "source_duration_sec": round(info["duration_sec"], 6),
                    "start_sample": start_sample,
                    "end_sample": end_sample,
                    "start_sec": round(start_sample / 16000.0, 6),
                    "end_sec": round(end_sample / 16000.0, 6),
                }
            )
            concat_lines.append(_concat_file_line(source_path))
            concat_lines.append(_concat_file_line(silence_paths["gap"]))
            cursor_samples += round(INTER_CLIP_SILENCE_SEC * 16000)
        concat_lines.append(_concat_file_line(silence_paths["trailing"]))
        cursor_samples += round(TRAILING_SILENCE_SEC * 16000)

        concat_path = work / "concat.txt"
        concat_path.write_text("".join(concat_lines), encoding="utf-8")
        temporary_audio = work / "physical-replay.flac"
        _run_ffmpeg(
            [
                "-f",
                "concat",
                "-safe",
                "0",
                "-i",
                str(concat_path),
                "-map",
                "0:a:0",
                "-ar",
                "16000",
                "-ac",
                "1",
                "-c:a",
                "flac",
                "-compression_level",
                "5",
                str(temporary_audio),
            ],
            purpose="rendering the one-hour replay",
        )
        rendered = flac_stream_info(temporary_audio)
        if rendered["total_samples"] != cursor_samples:
            raise RuntimeError(
                "rendered replay sample count mismatch: "
                f"{rendered['total_samples']} != {cursor_samples}"
            )
        temporary_audio.replace(audio_path)
    return cues, cursor_samples


def build_bundle(
    dataset_root: Path,
    evaluation_dir: Path | None,
    output_dir: Path,
    *,
    target_speech_sec: float = TARGET_SPEECH_SEC,
) -> dict:
    dataset_root = Path(dataset_root).resolve()
    evaluation_dir = Path(evaluation_dir).resolve() if evaluation_dir is not None else None
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    cue_path = output_dir / "cue-sheet.json"
    audio_path = output_dir / "physical-replay.flac"
    if cue_path.exists() or audio_path.exists():
        raise FileExistsError(
            f"physical bundle already exists at {output_dir}; use a new/empty directory"
        )

    if evaluation_dir is not None:
        clips = read_manifest(evaluation_dir / "manifest.jsonl")
        source = validate_frozen_evaluation(evaluation_dir, clips)
        rows = _latest_ok_jsonl(evaluation_dir / "streaming_results.jsonl")
        candidates = build_candidates(clips, rows)
        selection_metadata_source = "frozen_streaming_duration_and_reference_speed"
    else:
        clips = build_mediaspeech_manifest(dataset_root)
        candidates = build_candidates_from_dataset(clips, dataset_root)
        source = {
            "mode": "local_dataset_only_no_inference",
            "dataset_archive_sha256": ARCHIVE_SHA256,
            "dataset_manifest_sha256": manifest_fingerprint(clips),
            "clips": len(clips),
        }
        selection_metadata_source = "flac_streaminfo_duration_and_human_reference_speed"
    selected = select_representative(candidates, target_speech_sec)
    cues, cursor_samples = _render_replay_audio(selected, dataset_root, audio_path)

    speech_sec = sum(cue["source_duration_sec"] for cue in cues)
    payload = {
        "schema_version": SCHEMA_VERSION,
        "bundle_id": BUNDLE_ID,
        "dataset_id": DATASET_ID,
        "dataset_version": DATASET_VERSION,
        "reference_kind": "human_mediaspeech_double_annotated",
        "selection": {
            "algorithm": "duration_proportional_fixed_duration_speed_strata_sha256_order",
            "metadata_source": selection_metadata_source,
            "seed": SELECTION_SEED,
            "target_speech_sec": target_speech_sec,
            "selected_clips": len(cues),
            "selected_speech_sec": round(speech_sec, 6),
            "fingerprint": selection_fingerprint(selected),
        },
        "silence": {
            "leading_sec": LEADING_SILENCE_SEC,
            "between_clips_sec": INTER_CLIP_SILENCE_SEC,
            "trailing_sec": TRAILING_SILENCE_SEC,
        },
        "audio": {
            "file": audio_path.name,
            "sample_rate_hz": 16000,
            "channels": 1,
            "samples": cursor_samples,
            "duration_sec": round(cursor_samples / 16000.0, 6),
            "sha256": sha256_file(audio_path),
        },
        "source_evaluation": source,
        "selected_config": fixed_nemotron_config().as_effective_config(),
        "cues": cues,
    }
    _write_json(cue_path, payload)
    _write_json(
        output_dir / "bundle.json",
        {
            "schema_version": SCHEMA_VERSION,
            "bundle_id": BUNDLE_ID,
            "cue_sheet": cue_path.name,
            "cue_sheet_sha256": sha256_file(cue_path),
            "audio_file": audio_path.name,
            "audio_sha256": payload["audio"]["sha256"],
            "selection_fingerprint": payload["selection"]["fingerprint"],
        },
    )
    return payload


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-root", type=Path, required=True)
    parser.add_argument(
        "--evaluation-dir",
        type=Path,
        help="optional frozen evaluation; omit for local dataset-only preparation",
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--target-speech-sec", type=float, default=TARGET_SPEECH_SEC)
    args = parser.parse_args(argv)
    result = build_bundle(
        args.dataset_root,
        args.evaluation_dir,
        args.output_dir,
        target_speech_sec=args.target_speech_sec,
    )
    print(
        f"Physical bundle ready: {result['selection']['selected_clips']} clips, "
        f"{result['selection']['selected_speech_sec'] / 60.0:.1f} min speech, "
        f"{result['audio']['duration_sec'] / 60.0:.1f} min wall audio"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
