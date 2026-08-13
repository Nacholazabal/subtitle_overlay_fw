#!/usr/bin/env python3
"""Run and analyze the fixed one-hour Nemotron replay through the real board path."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import wave
from datetime import datetime
from pathlib import Path
from typing import Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from server.audio_tests.short_audio import (
    DEFAULT_STREAM_URL,
    NEMOTRON_PROFILE,
    board_delivery_result,
    distribution,
    health_check,
    load_json,
    load_jsonl,
    pipeline_reliability_result,
    overlay_timeline,
    sha256_file,
    verify_effective_config,
    wait_for_file,
    write_json,
)
from server.evaluation.dataset_manifest import normalize_spanish_text
from server.evaluation.dataset import error_rate, fixed_nemotron_config
from server.evaluation.prepare_physical import BUNDLE_ID


LOG_ROOT = REPO_ROOT / "logs" / "physical-evals"
LATENCY_TARGET_SEC = 1.5
FIXED_OVERRIDES = {
    "latency_ms": 560,
    "stop_history_eou_ms": 600,
    "residue_tokens_at_end": 2,
    "target_lang": "es-ES",
}


def validate_bundle(bundle_dir: Path) -> tuple[dict, dict, Path]:
    bundle_dir = Path(bundle_dir).resolve()
    bundle = load_json(bundle_dir / "bundle.json")
    if bundle.get("bundle_id") != BUNDLE_ID:
        raise ValueError(f"unexpected physical bundle id: {bundle.get('bundle_id')!r}")
    cue_path = bundle_dir / str(bundle["cue_sheet"])
    audio_path = bundle_dir / str(bundle["audio_file"])
    if sha256_file(cue_path) != bundle.get("cue_sheet_sha256"):
        raise ValueError("physical cue-sheet SHA-256 mismatch")
    if sha256_file(audio_path) != bundle.get("audio_sha256"):
        raise ValueError("physical replay audio SHA-256 mismatch")
    cues = load_json(cue_path)
    if cues.get("bundle_id") != BUNDLE_ID:
        raise ValueError("cue-sheet belongs to a different bundle")
    if cues.get("selected_config") != fixed_nemotron_config().as_effective_config():
        raise ValueError("physical bundle is not fixed at Nemotron 560/600/2 es-ES")
    if not cues.get("cues"):
        raise ValueError("physical cue-sheet contains no clips")
    return bundle, cues, audio_path


def _resample_for_alignment(samples, source_rate: int, target_rate: int = 1000):
    import numpy as np

    samples = np.asarray(samples, dtype="float32")
    if samples.size == 0:
        return samples
    count = int(round(samples.size * target_rate / float(source_rate)))
    source_x = np.arange(samples.size, dtype="float64") / float(source_rate)
    target_x = np.arange(count, dtype="float64") / float(target_rate)
    return np.interp(target_x, source_x, samples).astype("float32")


def _read_wave_region(path: Path, start_sec: float, duration_sec: float):
    import numpy as np

    with wave.open(str(path), "rb") as handle:
        rate = handle.getframerate()
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        if width != 2:
            raise ValueError(f"expected S16 board WAV, found sample width {width}")
        start_frame = max(0, int(round(start_sec * rate)))
        handle.setpos(min(start_frame, handle.getnframes()))
        frames = handle.readframes(max(0, int(round(duration_sec * rate))))
    audio = np.frombuffer(frames, dtype="<i2").astype("float32") / 32768.0
    if channels > 1 and audio.size:
        audio = audio.reshape(-1, channels).mean(axis=1)
    return audio, rate


def _read_replay_region(path: Path, start_sec: float, duration_sec: float):
    """Decode a small mono 16 kHz window using the ffmpeg already paired with ffplay."""
    import numpy as np

    completed = subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-ss",
            f"{start_sec:.6f}",
            "-t",
            f"{duration_sec:.6f}",
            "-i",
            str(path),
            "-ac",
            "1",
            "-ar",
            "16000",
            "-f",
            "f32le",
            "pipe:1",
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    return np.frombuffer(completed.stdout, dtype="<f4").copy()


def estimate_capture_alignment(
    replay_audio: Path,
    board_wav: Path,
    cue_sheet: dict,
    *,
    expected_offset_sec: float,
    search_radius_sec: float = 4.0,
) -> dict:
    """Cross-correlate known replay speech with captured board audio.

    Returns the board-audio clock position corresponding to replay time zero.
    Wall clocks provide only the search centre; the waveform supplies the final
    offset used to assign transcripts to human-labelled clips.
    """
    try:
        import numpy as np
    except ImportError as exc:
        raise RuntimeError("numpy is required for physical alignment") from exc

    first = cue_sheet["cues"][0]
    reference_start = float(first["start_sec"]) + min(0.5, float(first["source_duration_sec"]) / 10.0)
    reference_duration = min(6.0, float(first["end_sec"]) - reference_start)
    if reference_duration < 2.0:
        raise ValueError("first selected clip is too short for waveform alignment")
    reference = _resample_for_alignment(
        _read_replay_region(replay_audio, reference_start, reference_duration), 16000
    )

    expected_reference_capture = expected_offset_sec + reference_start
    search_start = max(0.0, expected_reference_capture - search_radius_sec)
    search_duration = reference_duration + 2.0 * search_radius_sec
    captured, capture_rate = _read_wave_region(board_wav, search_start, search_duration)
    search = _resample_for_alignment(captured, capture_rate)
    if search.size < reference.size:
        raise ValueError("captured board WAV is too short for waveform alignment")

    reference = reference - float(reference.mean())
    search = search - float(search.mean())
    reference_norm = float(np.linalg.norm(reference))
    if reference_norm <= 1e-6:
        raise ValueError("alignment reference contains no usable signal")
    correlation = np.correlate(search, reference, mode="valid")
    best = int(np.argmax(correlation))
    window = search[best : best + reference.size]
    denominator = reference_norm * float(np.linalg.norm(window))
    score = float(correlation[best] / denominator) if denominator > 1e-9 else 0.0
    offset = search_start + best / 1000.0 - reference_start
    return {
        "method": "waveform_normalized_cross_correlation_1khz",
        "capture_offset_sec": round(offset, 6),
        "expected_offset_from_wall_clock_sec": round(expected_offset_sec, 6),
        "correction_vs_wall_clock_sec": round(offset - expected_offset_sec, 6),
        "correlation_score": round(score, 6),
        "valid": score >= 0.25,
        "reference_replay_start_sec": round(reference_start, 6),
        "reference_duration_sec": round(reference_duration, 6),
        "search_radius_sec": search_radius_sec,
    }


def sequence_is_valid(events: Sequence[dict]) -> bool:
    sequences = [event.get("seq") for event in events]
    return sequences == list(range(len(sequences)))


def assign_final_events(events: Sequence[dict], cues: Sequence[dict], capture_offset_sec: float):
    assignments: dict[str, list[dict]] = {str(cue["clip_id"]): [] for cue in cues}
    unassigned: list[dict] = []
    for event in events:
        if not event.get("is_final"):
            continue
        start = event.get("start_sec")
        end = event.get("end_sec")
        if not isinstance(start, (int, float)) or not isinstance(end, (int, float)):
            unassigned.append(event)
            continue
        event_start, event_end = sorted((float(start), float(end)))
        best_cue = None
        best_overlap = 0.0
        for cue in cues:
            cue_start = capture_offset_sec + float(cue["start_sec"])
            cue_end = capture_offset_sec + float(cue["end_sec"])
            overlap = max(0.0, min(event_end, cue_end) - max(event_start, cue_start))
            if overlap > best_overlap:
                best_overlap = overlap
                best_cue = cue
        if best_cue is None:
            containing = [
                cue
                for cue in cues
                if capture_offset_sec + float(cue["start_sec"])
                <= event_end
                <= capture_offset_sec + float(cue["end_sec"])
            ]
            best_cue = containing[-1] if containing else None
        if best_cue is None:
            # A short final tail may be emitted by EOU during the controlled
            # gap. Assign it to the immediately preceding clip, never the next.
            preceding = [
                cue
                for cue in cues
                if 0.0 <= event_end - (capture_offset_sec + float(cue["end_sec"])) <= 1.2
            ]
            best_cue = preceding[-1] if preceding else None
        if best_cue is None:
            unassigned.append(event)
        else:
            assignments[str(best_cue["clip_id"])].append(event)
    return assignments, unassigned


def _micro(rows: Sequence[dict], key: str) -> dict:
    edits = sum(int(row[key]["edits"]) for row in rows)
    references = sum(int(row[key]["reference_units"]) for row in rows)
    hypotheses = sum(int(row[key]["hypothesis_units"]) for row in rows)
    rate = edits / references if references else (0.0 if hypotheses == 0 else 1.0)
    return {
        "edits": edits,
        "reference_units": references,
        "hypothesis_units": hypotheses,
        "rate": round(rate, 8),
    }


def _accepted_latency(events: Sequence[dict], acknowledgements: Sequence[dict]) -> dict:
    event_by_seq = {event.get("seq"): event for event in events}
    ack_transport: list[float] = []
    audio_to_board: list[float] = []
    accepted_sequences: set[int] = set()
    for ack in acknowledgements:
        if ack.get("status") != "accepted" or not isinstance(ack.get("seq"), int):
            continue
        seq = int(ack["seq"])
        accepted_sequences.add(seq)
        if isinstance(ack.get("ack_latency_sec"), (int, float)):
            ack_transport.append(float(ack["ack_latency_sec"]))
        event = event_by_seq.get(seq)
        if event and isinstance(event.get("bridge_audio_available_wall_sec"), (int, float)) \
                and isinstance(ack.get("ack_wall_sec"), (int, float)):
            audio_to_board.append(
                max(0.0, float(ack["ack_wall_sec"]) - float(event["bridge_audio_available_wall_sec"]))
            )
    return {
        "accepted_sequences": accepted_sequences,
        "ack_transport_sec": distribution(ack_transport),
        "audio_available_to_board_accepted_sec": distribution(audio_to_board),
        "within_1_5_sec_percent": (
            round(100.0 * sum(value <= LATENCY_TARGET_SEC for value in audio_to_board) / len(audio_to_board), 2)
            if audio_to_board
            else None
        ),
    }


def _wave_duration_sec(path: Path) -> float:
    with wave.open(str(path), "rb") as handle:
        return handle.getnframes() / float(handle.getframerate())


def captured_audio_metrics(
    path: Path, start_sec: float = 0.0, end_sec: float | None = None
) -> dict:
    """Vectorized level/clipping metrics suitable for an hour-long S16 WAV."""
    import numpy as np

    with wave.open(str(path), "rb") as handle:
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        rate = handle.getframerate()
        total_frames = handle.getnframes()
        if width != 2:
            return {"available": True, "sample_width_bits": width * 8, "unsupported": True}
        start_frame = max(0, int(round(float(start_sec) * rate)))
        end_frame = min(
            total_frames,
            int(round(float(end_sec) * rate)) if end_sec is not None else total_frames,
        )
        handle.setpos(min(start_frame, total_frames))
        remaining = max(0, end_frame - start_frame)
        window_frames = max(1, rate // 5)
        samples = 0
        sum_squares = 0.0
        peak = 0
        clipped = 0
        levels: list[float] = []
        while remaining > 0:
            count = min(remaining, window_frames)
            raw = handle.readframes(count)
            if not raw:
                break
            values = np.frombuffer(raw, dtype="<i2")
            if channels > 1:
                values = values.reshape(-1, channels)[:, 0]
            values64 = values.astype("float64")
            square_sum = float(np.dot(values64, values64))
            sample_count = int(values.size)
            samples += sample_count
            sum_squares += square_sum
            if sample_count:
                absolute = np.abs(values64)
                peak = max(peak, int(absolute.max()))
                clipped += int(np.count_nonzero(absolute >= 32765))
                rms = math.sqrt(square_sum / sample_count)
                levels.append(-math.inf if rms <= 0 else 20.0 * math.log10(rms / 32768.0))
            remaining -= count
    if samples == 0:
        return {"available": True, "duration_sec": 0.0, "empty": True}
    finite = np.asarray([value for value in levels if math.isfinite(value)], dtype="float64")
    floor = float(np.percentile(finite, 10)) if finite.size else None
    median = float(np.percentile(finite, 50)) if finite.size else None
    loud = float(np.percentile(finite, 90)) if finite.size else None
    dynamic_range = loud - floor if floor is not None and loud is not None else None
    rms = math.sqrt(sum_squares / samples)
    rms_dbfs = -math.inf if rms <= 0 else 20.0 * math.log10(rms / 32768.0)
    clipped_percent = 100.0 * clipped / samples
    peak_percent = 100.0 * peak / 32767.0
    if peak_percent < 5.0:
        verdict = "too_quiet"
    elif clipped_percent > 0.01:
        verdict = "clipping"
    elif floor is not None and dynamic_range is not None and floor > -35.0 and dynamic_range < 15.0:
        verdict = "noisy_floor"
    else:
        verdict = "ok"
    return {
        "available": True,
        "duration_sec": round(samples / rate, 3),
        "sample_rate_hz": rate,
        "channels": channels,
        "peak_percent": round(peak_percent, 3),
        "rms_dbfs": round(rms_dbfs, 3) if math.isfinite(rms_dbfs) else None,
        "clipped_percent": round(clipped_percent, 6),
        "floor_p10_dbfs": round(floor, 3) if floor is not None else None,
        "median_p50_dbfs": round(median, 3) if median is not None else None,
        "loud_p90_dbfs": round(loud, 3) if loud is not None else None,
        "dynamic_range_db": round(dynamic_range, 3) if dynamic_range is not None else None,
        "verdict": verdict,
    }


def completed_cues_for_capture(
    cues: Sequence[dict],
    *,
    capture_offset_sec: float,
    capture_duration_sec: float,
    closure_gap_sec: float,
) -> list[dict]:
    """Cues whose complete speech and following EOU silence were captured."""
    if closure_gap_sec < 0:
        raise ValueError("closure_gap_sec must be non-negative")
    replay_covered_sec = max(0.0, float(capture_duration_sec) - float(capture_offset_sec))
    return [
        cue
        for cue in cues
        if float(cue["end_sec"]) + float(closure_gap_sec) <= replay_covered_sec
    ]


def observed_board_delivery(
    events: Sequence[dict], acknowledgements: Sequence[dict], ready: dict
) -> dict:
    """Reconstruct delivery summary when an abrupt bridge failure skipped final stats."""
    explicit_rejections = {
        "rejected_old_seq",
        "dropped_event_pool",
        "dropped_subtitle_queue",
    }
    sessions = {
        ack.get("session_id")
        for ack in acknowledgements
        if isinstance(ack.get("session_id"), int)
    }
    delivery = {
        "handshake_ok": bool((ready.get("subtitle_session") or {}).get("handshake_ok")),
        "protocol_version": (ready.get("subtitle_session") or {}).get("protocol_version"),
        "session_id": max(sessions) if sessions else (ready.get("subtitle_session") or {}).get("session_id"),
        "connections": len(sessions),
        "reconnections": max(0, len(sessions) - 1),
        "generated": len(events),
        "sent": sum(isinstance(ack.get("sent_wall_sec"), (int, float)) for ack in acknowledgements),
        "accepted": sum(ack.get("status") == "accepted" for ack in acknowledgements),
        "rejected": sum(ack.get("status") in explicit_rejections for ack in acknowledgements),
        "delivery_unknown": sum(ack.get("status") == "delivery_unknown" for ack in acknowledgements),
        "sink_dropped_partials": sum(ack.get("status") == "sink_dropped_partial" for ack in acknowledgements),
        "sink_dropped_finals": sum(ack.get("status") == "sink_dropped_final" for ack in acknowledgements),
        "protocol_errors": sum(ack.get("status") == "delivery_unknown" for ack in acknowledgements),
        "queue_drained": False,
        "ack_latency_sec": [
            float(ack["ack_latency_sec"])
            for ack in acknowledgements
            if isinstance(ack.get("ack_latency_sec"), (int, float))
        ],
    }
    return board_delivery_result({"subtitle_delivery": delivery})


def observed_partial_pipeline(
    run_dir: Path, events: Sequence[dict], ready: dict, sequence_valid: bool
) -> dict:
    """Describe the usable prefix when abrupt shutdown prevented bridge-done.json."""
    start_drops = int(ready.get("board_dropped_chunks_start", 0))
    log_path = run_dir / "live" / "bridge.log"
    observed_drops = [
        int(value)
        for value in re.findall(
            r"\bdropped=(\d+)\b",
            log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else "",
        )
    ]
    end_drops = observed_drops[-1] if observed_drops else None
    dropped_during_scope = (
        max(0, end_drops - start_drops) if end_drops is not None else None
    )
    protocol_ok = bool(sequence_valid and dropped_during_scope == 0)
    return {
        "scope": "observed captured prefix before terminal WebSocket failure",
        "score": 100.0 if protocol_ok else None,
        "protocol_ok": protocol_ok,
        "chunks_forwarded": None,
        "board_dropped_chunks_start": start_drops,
        "board_dropped_chunks_end": end_drops,
        "board_dropped_chunks_during_session": dropped_during_scope,
        "board_dropped_chunks": dropped_during_scope,
        "jobs_submitted": None,
        "partial_jobs_skipped": None,
        "final_jobs_dropped": None,
        "dropped_audio_jobs": None,
        "events_emitted": len(events),
        "events_dropped": None,
        "incomplete_summary": True,
    }


def _joined_final_text(events: Sequence[dict]) -> str:
    return " ".join(
        str(event.get("full_text") or event.get("text") or "").strip()
        for event in events
        if str(event.get("full_text") or event.get("text") or "").strip()
    ).strip()


def _bitparallel_edit_distance(reference: Sequence[str], hypothesis: Sequence[str]) -> int:
    """Exact Levenshtein distance using Python integers as bit vectors.

    The regular dynamic-programming implementation is appropriate per clip, but
    becomes prohibitively slow for an hour-long character stream.  Myers' bit
    vector algorithm keeps the continuous partial-run analysis dependency-free.
    """
    length = len(reference)
    if length == 0:
        return len(hypothesis)
    masks: dict[str, int] = {}
    for index, unit in enumerate(reference):
        masks[unit] = masks.get(unit, 0) | (1 << index)
    limit = (1 << length) - 1
    positive = limit
    negative = 0
    distance = length
    last = 1 << (length - 1)
    for unit in hypothesis:
        equal = masks.get(unit, 0)
        vertical = equal | negative
        horizontal = (((equal & positive) + positive) ^ positive) | equal
        positive_horizontal = negative | ~(horizontal | positive)
        negative_horizontal = positive & horizontal
        if positive_horizontal & last:
            distance += 1
        elif negative_horizontal & last:
            distance -= 1
        positive_horizontal = ((positive_horizontal << 1) | 1) & limit
        negative_horizontal = (negative_horizontal << 1) & limit
        positive = (negative_horizontal | ~(vertical | positive_horizontal)) & limit
        negative = positive_horizontal & vertical
    return distance


def _continuous_error(reference_text: str, hypothesis_text: str, *, unit: str) -> dict:
    reference_normalized = normalize_spanish_text(reference_text)
    hypothesis_normalized = normalize_spanish_text(hypothesis_text)
    if unit == "word":
        reference = reference_normalized.split()
        hypothesis = hypothesis_normalized.split()
    elif unit == "character":
        reference = list(reference_normalized.replace(" ", ""))
        hypothesis = list(hypothesis_normalized.replace(" ", ""))
    else:
        raise ValueError(f"unsupported error-rate unit: {unit}")
    edits = _bitparallel_edit_distance(reference, hypothesis)
    rate = edits / len(reference) if reference else (0.0 if not hypothesis else 1.0)
    return {
        "edits": edits,
        "reference_units": len(reference),
        "hypothesis_units": len(hypothesis),
        "rate": round(rate, 8),
    }


def continuous_accuracy(
    cues: Sequence[dict], events: Sequence[dict], accepted_sequences: set[int]
) -> dict:
    """Score a continuous transcript without assuming model finals match cue cuts."""
    finals = sorted(
        (event for event in events if event.get("is_final")),
        key=lambda event: int(event.get("seq", -1)),
    )
    accepted_finals = [
        event for event in finals if event.get("seq") in accepted_sequences
    ]
    reference = " ".join(str(cue["reference_raw"]).strip() for cue in cues).strip()
    hypothesis = _joined_final_text(finals)
    accepted_hypothesis = _joined_final_text(accepted_finals)
    return {
        "mode": "continuous_transcript",
        "scope_note": (
            "Las referencias y los finales del modelo se concatenan cronológicamente. "
            "Se omite el WER por clip porque los rollups del modelo cruzan sus límites."
        ),
        "clips": len(cues),
        "micro_wer_vs_human": _continuous_error(reference, hypothesis, unit="word"),
        "micro_cer_vs_human": _continuous_error(reference, hypothesis, unit="character"),
        "board_confirmed_micro_wer_vs_human": _continuous_error(
            reference, accepted_hypothesis, unit="word"
        ),
        "board_confirmed_micro_cer_vs_human": _continuous_error(
            reference, accepted_hypothesis, unit="character"
        ),
        "macro_wer_vs_human": distribution([]),
        "perfect_clips": None,
        "empty_hypotheses": int(not normalize_spanish_text(hypothesis)),
        "board_confirmed_empty_hypotheses": int(
            not normalize_spanish_text(accepted_hypothesis)
        ),
        "by_stratum": {},
        "reference_words": len(normalize_spanish_text(reference).split()),
        "hypothesis_words": len(normalize_spanish_text(hypothesis).split()),
        "board_confirmed_hypothesis_words": len(
            normalize_spanish_text(accepted_hypothesis).split()
        ),
        "final_events": len(finals),
        "board_confirmed_final_events": len(accepted_finals),
    }


def analyze_run(
    run_dir: Path,
    cue_sheet: dict,
    replay_audio: Path,
    manifest: dict,
    *,
    allow_partial: bool = False,
) -> dict:
    events = load_jsonl(run_dir / "live" / "events.jsonl")
    acks = load_jsonl(run_dir / "live" / "board_acks.jsonl")
    ready = load_json(run_dir / "bridge-ready.json")
    done = load_json(run_dir / "bridge-done.json")
    board_wav = run_dir / "live" / "board_audio.wav"
    expected_offset = float(manifest["play_start_wall_sec"]) - float(ready["audio_start_wall_sec"])
    alignment = estimate_capture_alignment(
        replay_audio, board_wav, cue_sheet, expected_offset_sec=expected_offset
    )
    capture_duration_sec = _wave_duration_sec(board_wav)
    closure_gap_sec = float(cue_sheet.get("silence", {}).get("between_clips_sec", 0.0))
    # A failed partial run cannot use a weak waveform match to decide coverage.
    # Wall-clock timestamps still give a conservative and reproducible prefix;
    # the required post-cue silence keeps the cut away from an unfinished cue.
    coverage_offset = expected_offset if allow_partial else float(alignment["capture_offset_sec"])
    completed_cues = completed_cues_for_capture(
        cue_sheet["cues"],
        capture_offset_sec=coverage_offset,
        capture_duration_sec=capture_duration_sec,
        closure_gap_sec=closure_gap_sec,
    )
    analysis_cues = completed_cues if allow_partial else list(cue_sheet["cues"])
    completed_ids = {str(cue["clip_id"]) for cue in completed_cues}
    analysis_cutoff_sec = (
        coverage_offset + float(completed_cues[-1]["end_sec"]) + closure_gap_sec
        if allow_partial and completed_cues
        else capture_duration_sec
    )
    analysis_events = (
        [
            event
            for event in events
            if isinstance(event.get("end_sec"), (int, float))
            and float(event["end_sec"]) <= analysis_cutoff_sec
        ]
        if allow_partial
        else events
    )
    analysis_sequences = {
        int(event["seq"])
        for event in analysis_events
        if isinstance(event.get("seq"), int)
    }
    analysis_acks = (
        [ack for ack in acks if ack.get("seq") in analysis_sequences]
        if allow_partial
        else acks
    )
    assignments, unassigned = assign_final_events(
        analysis_events, cue_sheet["cues"], float(alignment["capture_offset_sec"])
    )
    latency = _accepted_latency(analysis_events, analysis_acks)
    accepted_sequences = latency["accepted_sequences"]

    per_clip: list[dict] = []
    for cue in ([] if allow_partial else analysis_cues):
        clip_events = sorted(assignments[str(cue["clip_id"])], key=lambda event: event.get("seq", -1))
        accepted_events = [
            event for event in clip_events if event.get("seq") in accepted_sequences
        ]
        hypothesis = _joined_final_text(clip_events)
        confirmed_hypothesis = _joined_final_text(accepted_events)
        per_clip.append(
            {
                "clip_id": cue["clip_id"],
                "stratum": cue["stratum"],
                "reference_raw": cue["reference_raw"],
                "hypothesis_raw": hypothesis,
                "hypothesis_normalized": normalize_spanish_text(hypothesis),
                "board_confirmed_hypothesis_raw": confirmed_hypothesis,
                "board_confirmed_hypothesis_normalized": normalize_spanish_text(
                    confirmed_hypothesis
                ),
                "wer_vs_human": error_rate(cue["reference_raw"], hypothesis, unit="word").as_dict(),
                "cer_vs_human": error_rate(cue["reference_raw"], hypothesis, unit="character").as_dict(),
                "board_confirmed_wer_vs_human": error_rate(
                    cue["reference_raw"], confirmed_hypothesis, unit="word"
                ).as_dict(),
                "board_confirmed_cer_vs_human": error_rate(
                    cue["reference_raw"], confirmed_hypothesis, unit="character"
                ).as_dict(),
                "final_event_count": len(clip_events),
                "board_confirmed_final_event_count": len(accepted_events),
                "event_sequences": [event.get("seq") for event in clip_events],
            }
        )

    ack_by_seq = {
        int(ack["seq"]): ack
        for ack in acks
        if ack.get("status") == "accepted" and isinstance(ack.get("seq"), int)
    }
    cue_by_id = {str(cue["clip_id"]): cue for cue in cue_sheet["cues"]}
    first_subtitle: list[float] = []
    audio_start_wall = float(ready["audio_start_wall_sec"])
    for clip_id, clip_events in ([] if allow_partial else assignments.items()):
        if clip_id not in completed_ids:
            continue
        accepted = [
            ack_by_seq[event["seq"]]
            for event in clip_events
            if isinstance(event.get("seq"), int) and event["seq"] in ack_by_seq
        ]
        if accepted:
            cue_start_clock = float(alignment["capture_offset_sec"]) + float(cue_by_id[clip_id]["start_sec"])
            first_subtitle.append(
                max(0.0, min(float(ack["ack_wall_sec"]) for ack in accepted) - (audio_start_wall + cue_start_clock))
            )

    strata: dict[str, list[dict]] = {}
    for row in per_clip:
        strata.setdefault(row["stratum"], []).append(row)
    accuracy_by_stratum = {
        name: {
            "clips": len(rows),
            "micro_wer_vs_human": _micro(rows, "wer_vs_human"),
            "micro_cer_vs_human": _micro(rows, "cer_vs_human"),
            "board_confirmed_micro_wer_vs_human": _micro(
                rows, "board_confirmed_wer_vs_human"
            ),
            "board_confirmed_micro_cer_vs_human": _micro(
                rows, "board_confirmed_cer_vs_human"
            ),
        }
        for name, rows in sorted(strata.items())
    }
    accuracy = (
        continuous_accuracy(analysis_cues, analysis_events, accepted_sequences)
        if allow_partial
        else {
            "mode": "per_clip_aligned",
            "scope_note": "Final events are assigned to waveform-aligned cue intervals.",
            "clips": len(per_clip),
            "micro_wer_vs_human": _micro(per_clip, "wer_vs_human"),
            "micro_cer_vs_human": _micro(per_clip, "cer_vs_human"),
            "board_confirmed_micro_wer_vs_human": _micro(
                per_clip, "board_confirmed_wer_vs_human"
            ),
            "board_confirmed_micro_cer_vs_human": _micro(
                per_clip, "board_confirmed_cer_vs_human"
            ),
            "macro_wer_vs_human": distribution(
                [row["wer_vs_human"]["rate"] for row in per_clip]
            ),
            "perfect_clips": sum(row["wer_vs_human"]["edits"] == 0 for row in per_clip),
            "empty_hypotheses": sum(not row["hypothesis_normalized"] for row in per_clip),
            "board_confirmed_empty_hypotheses": sum(
                not row["board_confirmed_hypothesis_normalized"] for row in per_clip
            ),
            "by_stratum": accuracy_by_stratum,
        }
    )

    reasons: dict[str, int] = {}
    for event in analysis_events:
        reason = event.get("final_reason")
        if event.get("is_final") and isinstance(reason, str):
            reasons[reason] = reasons.get(reason, 0) + 1

    logical_overlay = overlay_timeline(
        [
            event
            for event in analysis_events
            if event.get("seq") in latency["accepted_sequences"]
        ]
    )
    visible_durations = [
        float(record["visible_duration_sec"])
        for record in logical_overlay
        if isinstance(record.get("visible_duration_sec"), (int, float))
    ]
    capture_start = max(0.0, coverage_offset)
    capture_end = min(
        capture_duration_sec,
        capture_start + float(cue_sheet["audio"]["duration_sec"]),
    )
    captured_audio = captured_audio_metrics(board_wav, capture_start, capture_end)

    sequence_valid = sequence_is_valid(analysis_events)
    pipeline = (
        observed_partial_pipeline(run_dir, analysis_events, ready, sequence_valid)
        if allow_partial
        else pipeline_reliability_result(done, sequence_valid)
    )
    board = (
        board_delivery_result(done)
        if not allow_partial
        and isinstance(done.get("subtitle_delivery"), dict)
        and done["subtitle_delivery"]
        else observed_board_delivery(analysis_events, analysis_acks, ready)
    )
    fully_covered = len(completed_cues) == len(cue_sheet["cues"])
    valid = bool(
        alignment["valid"]
        and sequence_valid
        and fully_covered
        and pipeline["protocol_ok"]
        and board["protocol_ok"]
        and not unassigned
    )
    partial_usable = bool(
        allow_partial
        and sequence_valid
        and completed_cues
    )
    status = "valid" if valid else "partial_usable_transport_invalid" if partial_usable else "invalid"
    completed_speech_sec = sum(float(cue["source_duration_sec"]) for cue in completed_cues)
    return {
        "schema_version": 1,
        "run_id": run_dir.name,
        "status": status,
        "reference_kind": cue_sheet["reference_kind"],
        "bundle_id": cue_sheet["bundle_id"],
        "selection": cue_sheet["selection"],
        "config": done.get("run_config", ready.get("run_config")),
        "alignment": alignment,
        "coverage": {
            "scope": "fully captured cues including their following EOU silence",
            "captured_audio_sec": round(capture_duration_sec, 3),
            "replay_covered_sec": round(max(0.0, capture_duration_sec - coverage_offset), 3),
            "coverage_offset_sec": round(coverage_offset, 6),
            "coverage_offset_source": "wall_clock" if allow_partial else "waveform_alignment",
            "analysis_cutoff_capture_sec": round(analysis_cutoff_sec, 6),
            "completed_clips": len(completed_cues),
            "total_bundle_clips": len(cue_sheet["cues"]),
            "completed_speech_sec": round(completed_speech_sec, 3),
            "completed_speech_min": round(completed_speech_sec / 60.0, 3),
            "excluded_incomplete_clips": len(cue_sheet["cues"]) - len(completed_cues),
            "full_bundle_captured": fully_covered,
            "accuracy_scope_valid": bool(completed_cues and sequence_valid),
            "transport_scope_valid": valid,
        },
        "accuracy": accuracy,
        "latency": {
            "definition": "event audio available at bridge to transcript accepted by firmware",
            "target_sec": LATENCY_TARGET_SEC,
            "bridge_receive_sec": distribution([
                float(event["bridge_receive_lag_sec"])
                for event in analysis_events
                if isinstance(event.get("bridge_receive_lag_sec"), (int, float))
            ]),
            "ack_transport_sec": latency["ack_transport_sec"],
            "audio_available_to_board_accepted_sec": latency[
                "audio_available_to_board_accepted_sec"
            ],
            "within_target_percent": latency["within_1_5_sec_percent"],
            "first_subtitle_from_clip_start_sec": distribution(first_subtitle),
            "word_timing_warning": (
                "Clip-start latency is not exact spoken-word latency; the model does not emit "
                "forced-aligned word onset timestamps."
            ),
        },
        "reliability": {
            "valid": valid,
            "event_sequence_valid": sequence_valid,
            "pipeline": pipeline,
            "board_delivery": board,
            "unassigned_final_events": len(unassigned),
            "unassigned_final_sequences": [event.get("seq") for event in unassigned],
            "clip_assignment_performed": not allow_partial,
        },
        "segmentation": {
            "final_reasons": reasons,
            "events": len(analysis_events),
            "finals": sum(bool(event.get("is_final")) for event in analysis_events),
            "partials": sum(not bool(event.get("is_final")) for event in analysis_events),
        },
        "readability": {
            "definition": "logical firmware overlay state duration between accepted production events",
            "target_visible_sec": 1.5,
            "visible_duration_sec": distribution(visible_durations),
            "states_at_least_target_percent": (
                round(100.0 * sum(value >= 1.5 for value in visible_durations) / len(visible_durations), 2)
                if visible_durations
                else None
            ),
            "logical_only": True,
        },
        "captured_audio": captured_audio,
        "per_clip": per_clip,
        "artifacts": {
            "events": "live/events.jsonl",
            "board_acks": "live/board_acks.jsonl",
            "board_audio": "live/board_audio.wav",
            "bridge_log": "live/bridge.log",
        },
        "scope_warning": (
            "Firmware ACK accepted proves reception and queueing by SttAO/SubtitleAO; "
            "it does not prove physical HDMI pixels."
        ),
        "_overlay_timeline": logical_overlay,
    }


def _fmt(value, digits: int = 3) -> str:
    return "n/a" if value is None else f"{float(value):.{digits}f}"


def render_report(report: dict) -> str:
    accuracy = report["accuracy"]
    coverage = report["coverage"]
    latency = report["latency"]
    reliability = report["reliability"]
    board = reliability["board_delivery"]
    end_to_end = latency["audio_available_to_board_accepted_sec"]
    lines = [
        "# Evaluación física Nemotron — MediaSpeech ES",
        "",
        f"Estado: **{report['status']}**",
        "",
        "## Resultado principal",
        "",
        f"- Cobertura utilizable: {coverage['completed_clips']}/{coverage['total_bundle_clips']} clips; "
        f"{coverage['completed_speech_min']:.2f} min de habla completamente capturada",
        f"- Audio capturado / replay cubierto: {coverage['captured_audio_sec']:.2f} / "
        f"{coverage['replay_covered_sec']:.2f} s",
        f"- WER/CER micro humano generado por Nemotron: "
        f"{100.0 * accuracy['micro_wer_vs_human']['rate']:.2f}% / "
        f"{100.0 * accuracy['micro_cer_vs_human']['rate']:.2f}%",
        f"- WER/CER micro confirmado por ACK de placa: "
        f"{100.0 * accuracy['board_confirmed_micro_wer_vs_human']['rate']:.2f}% / "
        f"{100.0 * accuracy['board_confirmed_micro_cer_vs_human']['rate']:.2f}%",
        f"- Modo de exactitud: `{accuracy['mode']}`",
        f"- {accuracy['scope_note']}",
        f"- Latencia audio-disponible→ACK p50/p90/p95/p99/máxima: "
        f"{_fmt(end_to_end['p50'])} / {_fmt(end_to_end['p90'])} / {_fmt(end_to_end['p95'])} / "
        f"{_fmt(end_to_end['p99'])} / {_fmt(end_to_end['max'])} s",
        f"- Eventos dentro de 1.5 s: {_fmt(latency['within_target_percent'], 2)}%",
        "",
        "## Entrega real a firmware",
        "",
        f"- Validez del alcance de exactitud / transporte completo: "
        f"{coverage['accuracy_scope_valid']} / {coverage['transport_scope_valid']}",
        f"- Handshake / secuencia: {board['handshake_ok']} / {reliability['event_sequence_valid']}",
        f"- Generados / enviados / aceptados: {board['generated']} / {board['sent']} / {board['accepted']}",
        f"- Rechazados / ACK ausentes / desconocidos: {board['rejected']} / "
        f"{board['missing_acks']} / {board['delivery_unknown']}",
        f"- Drops de audio durante sesión: {reliability['pipeline']['board_dropped_chunks_during_session']}",
        f"- Sesiones TCP de subtítulos / reconexiones: {board['connections']} / "
        f"{board['reconnections']}",
        f"- Finales sin asignar a cue: {reliability['unassigned_final_events']}",
        "- `accepted` prueba recepción y encolado por firmware; no prueba los píxeles HDMI.",
        "",
        "## Sincronización física",
        "",
        f"- Método: `{report['alignment']['method']}`",
        f"- Correlación: {_fmt(report['alignment']['correlation_score'])}",
        f"- Offset replay→captura: {_fmt(report['alignment']['capture_offset_sec'])} s",
        f"- Corrección respecto a wall-clock: {_fmt(report['alignment']['correction_vs_wall_clock_sec'])} s",
        f"- Cobertura calculada con: `{coverage['coverage_offset_source']}` "
        f"(offset {_fmt(coverage['coverage_offset_sec'])} s)",
        "",
        "## WER/CER por estrato",
        "",
        "| Duración / velocidad | Clips | WER micro | CER micro |",
        "| --- | ---: | ---: | ---: |",
    ]
    for name, row in accuracy["by_stratum"].items():
        lines.append(
            f"| `{name}` | {row['clips']} | {100.0 * row['micro_wer_vs_human']['rate']:.2f}% "
            f"| {100.0 * row['micro_cer_vs_human']['rate']:.2f}% |"
        )
    if not accuracy["by_stratum"]:
        lines.append("| No disponible para transcript continuo | — | — | — |")
    reasons = report["segmentation"]["final_reasons"]
    lines.extend(
        [
            "",
            "## Segmentación y alcance",
            "",
            f"- Eventos / finales / parciales: {report['segmentation']['events']} / "
            f"{report['segmentation']['finals']} / {report['segmentation']['partials']}",
            f"- Razones finales: `{json.dumps(reasons, sort_keys=True)}`",
            f"- Estados lógicos visibles ≥1.5 s: "
            f"{_fmt(report['readability']['states_at_least_target_percent'], 2)}%",
            f"- Audio capturado: `{report['captured_audio'].get('verdict', 'n/a')}`; pico "
            f"{_fmt(report['captured_audio'].get('peak_percent'))}%; clipping "
            f"{_fmt(report['captured_audio'].get('clipped_percent'), 4)}%",
            f"- {latency['word_timing_warning']}",
            f"- {report['scope_warning']}",
            "",
            "El WER/CER parcial concatena únicamente las referencias de clips cuyo audio completo "
            "y silencio de cierre quedaron dentro del WAV capturado. La hipótesis concatena los "
            "finales cronológicos del mismo prefijo. Los clips posteriores y el clip cortado se "
            "excluyen; no se fuerza una segmentación por clip que el modelo no produjo.",
            "",
            "La métrica de 1.5 s usa el final de audio que el evento declara como disponible en "
            "el bridge hasta el ACK de aceptación de la placa. Es la medición automática "
            "end-to-end más precisa disponible sin forced alignment por palabra ni captura HDMI.",
            "",
        ]
    )
    return "\n".join(lines)


def _new_run_dir() -> Path:
    base = datetime.now().strftime("%Y%m%d-%H%M%S")
    candidate = LOG_ROOT / base
    suffix = 1
    while candidate.exists():
        candidate = LOG_ROOT / f"{base}-{suffix}"
        suffix += 1
    candidate.mkdir(parents=True)
    return candidate


def run_physical(args) -> int:
    bundle, cue_sheet, replay_audio = validate_bundle(args.bundle)
    if shutil.which("ffplay") is None:
        raise RuntimeError("ffplay is required but was not found in WSL PATH")
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("ffmpeg is required for replay/capture alignment")
    health = health_check(args.stream_url)
    NEMOTRON_PROFILE.verify_health(health)

    run_dir = _new_run_dir()
    live_dir = run_dir / "live"
    live_dir.mkdir()
    ready_file = run_dir / "bridge-ready.json"
    done_file = run_dir / "bridge-done.json"
    stop_file = run_dir / "bridge-stop"
    manifest = {
        "schema_version": 1,
        "run_id": run_dir.name,
        "status": "starting",
        "bundle_dir": str(Path(args.bundle).resolve()),
        "bundle": bundle,
        "stream_url": args.stream_url,
        "health": health,
        "requested_config": FIXED_OVERRIDES,
        "created_wall_sec": time.time(),
    }
    write_json(run_dir / "manifest.json", manifest)
    shutil.copy2(Path(args.bundle).resolve() / bundle["cue_sheet"], run_dir / "cue-sheet.json")
    shutil.copy2(Path(args.bundle).resolve() / "bundle.json", run_dir / "bundle.json")

    relative = lambda path: str(path.relative_to(REPO_ROOT)).replace(os.sep, "/")
    environment = os.environ.copy()
    environment.update(
        {
            "STT_STREAM_URL": args.stream_url,
            "STT_FOREGROUND": "1",
            "STT_SINGLE_SESSION": "1",
            "STT_JSONL": relative(live_dir / "events.jsonl"),
            "STT_BOARD_ACK_JSONL": relative(live_dir / "board_acks.jsonl"),
            "STT_SAVE_WAV": relative(live_dir / "board_audio.wav"),
            "STT_READY_FILE": relative(ready_file),
            "STT_DONE_FILE": relative(done_file),
            "STT_STOP_FILE": relative(stop_file),
            "STT_BACKEND_CONFIG_JSON": json.dumps(FIXED_OVERRIDES, separators=(",", ":")),
        }
    )
    bridge_log = (live_dir / "bridge.log").open("w", encoding="utf-8")
    bridge = subprocess.Popen(
        [str(REPO_ROOT / "server" / "run.sh")],
        cwd=REPO_ROOT,
        env=environment,
        stdout=bridge_log,
        stderr=subprocess.STDOUT,
        text=True,
    )

    interrupted = False

    def request_stop(_signum=None, _frame=None):
        nonlocal interrupted
        interrupted = True
        stop_file.touch(exist_ok=True)

    previous_sigint = signal.signal(signal.SIGINT, request_stop)
    previous_sigterm = signal.signal(signal.SIGTERM, request_stop)
    try:
        print(f"Waiting up to {args.ready_timeout:.0f}s for board + Colab + firmware ACK handshake ...")
        ready = wait_for_file(ready_file, bridge, args.ready_timeout)
        verify_effective_config(FIXED_OVERRIDES, ready.get("run_config", {}), NEMOTRON_PROFILE)
        manifest["bridge_ready"] = ready
        manifest["status"] = "playing"
        manifest["play_start_wall_sec"] = time.time()
        write_json(run_dir / "manifest.json", manifest)
        print(
            f"Playing fixed bundle: {cue_sheet['selection']['selected_clips']} clips, "
            f"{cue_sheet['audio']['duration_sec'] / 60.0:.1f} min"
        )
        result = subprocess.run(
            [
                "ffplay",
                "-hide_banner",
                "-loglevel",
                "error",
                "-nodisp",
                "-autoexit",
                "-volume",
                "100",
                str(replay_audio),
            ],
            check=False,
        )
        manifest["play_end_wall_sec"] = time.time()
        manifest["player_exit_code"] = result.returncode
        if result.returncode != 0:
            raise RuntimeError(f"ffplay failed with code {result.returncode}")
        if interrupted:
            raise KeyboardInterrupt
        time.sleep(args.post_play_sec)
        stop_file.touch(exist_ok=True)
        bridge.wait(timeout=60)
        if bridge.returncode != 0:
            raise RuntimeError(f"bridge exited with code {bridge.returncode}; inspect {live_dir / 'bridge.log'}")
        wait_for_file(done_file, bridge, 5)
        manifest["status"] = "analyzing"
        write_json(run_dir / "manifest.json", manifest)
        report = analyze_run(run_dir, cue_sheet, replay_audio, manifest)
        overlay = report.pop("_overlay_timeline")
        write_json(run_dir / "overlay-timeline.json", overlay)
        write_json(run_dir / "report.json", report)
        (run_dir / "report.md").write_text(render_report(report), encoding="utf-8")
        (run_dir / "visual-observation.md").write_text(
            "# Observación HDMI\n\n"
            "- [ ] Se vieron subtítulos durante toda la corrida.\n"
            "- [ ] El roll-up de tres líneas fue legible.\n"
            "- [ ] No hubo congelamientos o desapariciones prolongadas.\n"
            "- Observaciones / timestamps: \n",
            encoding="utf-8",
        )
        manifest["status"] = report["status"]
        manifest["finished_wall_sec"] = time.time()
        write_json(run_dir / "manifest.json", manifest)
        print(f"\nPHYSICAL EVALUATION: {report['status']}")
        print(f"  WER / CER    : {100 * report['accuracy']['micro_wer_vs_human']['rate']:.2f}% / "
              f"{100 * report['accuracy']['micro_cer_vs_human']['rate']:.2f}%")
        print(f"  board ACKs   : {report['reliability']['board_delivery']['accepted']}/"
              f"{report['reliability']['board_delivery']['generated']}")
        print(f"  latency p90  : {_fmt(report['latency']['audio_available_to_board_accepted_sec']['p90'])}s")
        print(f"  report       : {run_dir.relative_to(REPO_ROOT) / 'report.md'}")
        return 0 if report["status"] == "valid" else 2
    except KeyboardInterrupt:
        stop_file.touch(exist_ok=True)
        manifest["status"] = "interrupted"
        manifest["finished_wall_sec"] = time.time()
        write_json(run_dir / "manifest.json", manifest)
        return 130
    except Exception:
        stop_file.touch(exist_ok=True)
        manifest["status"] = "failed"
        manifest["finished_wall_sec"] = time.time()
        write_json(run_dir / "manifest.json", manifest)
        raise
    finally:
        signal.signal(signal.SIGINT, previous_sigint)
        signal.signal(signal.SIGTERM, previous_sigterm)
        if bridge.poll() is None:
            try:
                bridge.wait(timeout=15)
            except subprocess.TimeoutExpired:
                bridge.terminate()
        bridge_log.close()


def analyze_existing_run(run_dir: Path) -> int:
    """Create a scope-correct partial report without mutating original run evidence."""
    run_dir = Path(run_dir).resolve()
    manifest = load_json(run_dir / "manifest.json")
    bundle_dir = Path(str(manifest["bundle_dir"])).resolve()
    _bundle, cue_sheet, replay_audio = validate_bundle(bundle_dir)
    report = analyze_run(
        run_dir,
        cue_sheet,
        replay_audio,
        manifest,
        allow_partial=True,
    )
    overlay = report.pop("_overlay_timeline")
    write_json(run_dir / "partial-overlay-timeline.json", overlay)
    write_json(run_dir / "partial-report.json", report)
    (run_dir / "partial-report.md").write_text(render_report(report), encoding="utf-8")
    print(f"PARTIAL PHYSICAL EVALUATION: {report['status']}")
    print(
        f"  coverage     : {report['coverage']['completed_clips']}/"
        f"{report['coverage']['total_bundle_clips']} clips, "
        f"{report['coverage']['completed_speech_min']:.2f} min speech"
    )
    print(
        f"  model WER/CER: {100 * report['accuracy']['micro_wer_vs_human']['rate']:.2f}% / "
        f"{100 * report['accuracy']['micro_cer_vs_human']['rate']:.2f}%"
    )
    print(
        f"  ACK WER/CER  : "
        f"{100 * report['accuracy']['board_confirmed_micro_wer_vs_human']['rate']:.2f}% / "
        f"{100 * report['accuracy']['board_confirmed_micro_cer_vs_human']['rate']:.2f}%"
    )
    print(
        f"  board ACKs   : {report['reliability']['board_delivery']['accepted']}/"
        f"{report['reliability']['board_delivery']['generated']}"
    )
    print(f"  report       : {run_dir / 'partial-report.md'}")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    default_bundle = os.environ.get("NEMOTRON_PHYSICAL_BUNDLE")
    parser.add_argument("--bundle", type=Path, default=Path(default_bundle) if default_bundle else None)
    parser.add_argument(
        "--analyze-existing",
        type=Path,
        help="analyze a completed/interrupted physical run without replaying audio",
    )
    parser.add_argument("--stream-url", default=os.environ.get("STT_STREAM_URL", DEFAULT_STREAM_URL))
    parser.add_argument("--ready-timeout", type=float, default=120.0)
    parser.add_argument("--post-play-sec", type=float, default=2.0)
    args = parser.parse_args(argv)
    if args.analyze_existing is not None:
        return analyze_existing_run(args.analyze_existing)
    if args.bundle is None:
        parser.error("pass --bundle DIR or set NEMOTRON_PHYSICAL_BUNDLE")
    return run_physical(args)


if __name__ == "__main__":
    raise SystemExit(main())
