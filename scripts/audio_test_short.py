#!/usr/bin/env python3
"""Run and report the short-audio end-to-end STT benchmark."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import shutil
import signal
import struct
import subprocess
import sys
import time
import unicodedata
import urllib.error
import urllib.request
import wave
from datetime import datetime
from pathlib import Path
from urllib.parse import urlsplit, urlunsplit


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))
from scripts.stt_stream_protocol import validate_config_overrides


DEFAULT_AUDIO_DIR = Path("/mnt/c/Users/nacho/Desktop/Postgrado/TESIS/audios")
DEFAULT_STREAM_URL = "wss://passage-capacity-wistful.ngrok-free.dev/stt/stream"
EXPECTED_CLIPS = ("desay-short", "noticiero-short", "rel-short")
LOG_ROOT = REPO_ROOT / "logs" / "audio-tests"
CACHE_ROOT = LOG_ROOT / "cache" / "offline"
LATENCY_TARGET_SEC = 1.5
READABILITY_TARGET_SEC = 1.5
CLEAR_TIMEOUT_SEC = 5.0
HALLUCINATION_MARKERS = (
    "amara.org",
    "subtítulos realizados por",
    "subtitulos realizados por",
    "suscríbete al canal",
    "gracias por ver el video",
    "gracias por ver este video",
    "suscríbete",
    "suscribete",
    "got it",
)

SESSION_PARAMETER_MAP = {
    "max_window_sec": ("config_max_window_sec", "STT_MAX_WINDOW_SEC"),
    "min_silence_sec": ("config_min_silence_sec", "STT_MIN_SILENCE_SEC"),
    "partial_sec": ("config_partial_sec", "STT_PARTIAL_SEC"),
    "partial_agreement": ("config_partial_agreement", "STT_PARTIAL_AGREEMENT"),
    "gain": ("config_gain", "STT_GAIN"),
    "vad_threshold": ("config_vad_threshold", "STT_VAD_THRESHOLD"),
    "vad_neg_threshold": ("config_vad_neg_threshold", "STT_VAD_NEG_THRESHOLD"),
}

DEFAULT_SWEEP_CASES = (
    {
        "name": "baseline",
        "max_window_sec": 3.0,
        "min_silence_sec": 0.3,
        "partial_sec": 0.5,
        "partial_agreement": 1,
        "vad_threshold": 0.5,
        "vad_neg_threshold": 0.35,
    },
    {
        "name": "calmer_updates",
        "max_window_sec": 3.0,
        "min_silence_sec": 0.3,
        "partial_sec": 1.0,
        "partial_agreement": 1,
        "vad_threshold": 0.5,
        "vad_neg_threshold": 0.35,
    },
    {
        "name": "stable_partials",
        "max_window_sec": 3.0,
        "min_silence_sec": 0.3,
        "partial_sec": 1.0,
        "partial_agreement": 2,
        "vad_threshold": 0.5,
        "vad_neg_threshold": 0.35,
    },
    {
        "name": "wider_context",
        "max_window_sec": 4.0,
        "min_silence_sec": 0.5,
        "partial_sec": 1.0,
        "partial_agreement": 2,
        "vad_threshold": 0.5,
        "vad_neg_threshold": 0.35,
    },
    {
        "name": "sensitive_vad",
        "max_window_sec": 3.0,
        "min_silence_sec": 0.3,
        "partial_sec": 1.0,
        "partial_agreement": 2,
        "vad_threshold": 0.35,
        "vad_neg_threshold": 0.2,
    },
    {
        "name": "strict_vad",
        "max_window_sec": 3.0,
        "min_silence_sec": 0.3,
        "partial_sec": 1.0,
        "partial_agreement": 2,
        "vad_threshold": 0.65,
        "vad_neg_threshold": 0.5,
    },
)


def write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        parsed = json.load(handle)
    if not isinstance(parsed, dict):
        raise ValueError(f"expected JSON object in {path}")
    return parsed


def load_jsonl(path: Path) -> list[dict]:
    events: list[dict] = []
    if not path.exists():
        return events
    with path.open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            if not line.strip():
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"invalid JSONL at {path}:{line_number}: {exc}") from exc
            if isinstance(event, dict):
                events.append(event)
    return events


def requested_session_config(args) -> dict:
    requested = {
        name: value
        for name in SESSION_PARAMETER_MAP
        if (value := getattr(args, name, None)) is not None
    }
    return validate_config_overrides(requested)


def verify_effective_config(requested: dict, effective: dict) -> None:
    for name, expected in requested.items():
        config_key, _environment_key = SESSION_PARAMETER_MAP[name]
        actual = effective.get(config_key)
        if isinstance(expected, float):
            matches = isinstance(actual, (int, float)) and not isinstance(actual, bool)
            matches = matches and math.isclose(float(actual), expected, rel_tol=0.0, abs_tol=1e-6)
        else:
            matches = actual == expected
        if not matches:
            raise RuntimeError(
                f"Colab did not apply {name}={expected!r}; session reported {actual!r}"
            )


def counter_delta_for_interval(events: list[dict], field: str, start_wall: float, end_wall: float) -> int:
    baseline = 0
    maximum = None
    for event in sorted(events, key=lambda item: item.get("bridge_received_wall_sec", 0)):
        received = event.get("bridge_received_wall_sec")
        value = event.get(field)
        if not isinstance(received, (int, float)) or not isinstance(value, int):
            continue
        if received < start_wall:
            baseline = max(baseline, value)
        elif received <= end_wall:
            maximum = max(maximum if maximum is not None else baseline, value)
    return max(0, (maximum if maximum is not None else baseline) - baseline)


def http_base_from_stream_url(stream_url: str) -> str:
    parsed = urlsplit(stream_url)
    if parsed.scheme not in ("ws", "wss"):
        raise ValueError("STT_STREAM_URL must use ws:// or wss://")
    scheme = "https" if parsed.scheme == "wss" else "http"
    path = parsed.path
    if path.endswith("/stt/stream"):
        path = path[: -len("/stt/stream")]
    return urlunsplit((scheme, parsed.netloc, path.rstrip("/"), "", ""))


def request_json(url: str, data: bytes | None = None, headers: dict[str, str] | None = None) -> dict:
    request_headers = {
        "Accept": "application/json",
        "ngrok-skip-browser-warning": "true",
        **(headers or {}),
    }
    request = urllib.request.Request(url, data=data, headers=request_headers)
    try:
        with urllib.request.urlopen(request, timeout=180) as response:
            parsed = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code} from {url}: {detail}") from exc
    except (urllib.error.URLError, TimeoutError) as exc:
        raise RuntimeError(f"cannot reach Colab at {url}: {exc}") from exc
    if not isinstance(parsed, dict):
        raise RuntimeError(f"expected JSON object from {url}")
    return parsed


def health_check(stream_url: str) -> dict:
    health = request_json(http_base_from_stream_url(stream_url) + "/health")
    if health.get("status") != "ok":
        raise RuntimeError(f"Colab health check was not OK: {health}")
    return health


def discover_clips(audio_dir: Path) -> list[Path]:
    found = sorted(audio_dir.glob("*-short.webm"))
    names = tuple(path.stem for path in found)
    if names != EXPECTED_CLIPS:
        raise RuntimeError(
            f"expected exactly {EXPECTED_CLIPS} in {audio_dir}, found {names or 'none'}"
        )
    return found


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def offline_vad_overrides(session_config: dict | None) -> dict:
    return {
        key: value
        for key, value in (session_config or {}).items()
        if key in ("vad_threshold", "vad_neg_threshold")
    }


def offline_signature(health: dict, session_config: dict | None = None) -> dict:
    config = health.get("run_config", {})
    keys = (
        "config_model",
        "config_language",
        "config_device",
        "config_compute_type",
        "config_beam_size",
        "config_vad_filter",
        "config_vad_threshold",
        "config_vad_neg_threshold",
    )
    signature = {key: config.get(key) for key in keys}
    for name, value in offline_vad_overrides(session_config).items():
        signature[SESSION_PARAMETER_MAP[name][0]] = value
    return signature


def offline_reference(
    clip: Path,
    stream_url: str,
    health: dict,
    refresh: bool = False,
    session_config: dict | None = None,
) -> tuple[dict, Path, bool]:
    audio_hash = sha256_file(clip)
    signature = offline_signature(health, session_config)
    cache_material = json.dumps(
        {"audio_sha256": audio_hash, "config": signature}, sort_keys=True
    ).encode("utf-8")
    cache_key = hashlib.sha256(cache_material).hexdigest()
    cache_path = CACHE_ROOT / cache_key[:2] / f"{cache_key}.json"
    if cache_path.exists() and not refresh:
        return load_json(cache_path), cache_path, True

    result = request_json(
        http_base_from_stream_url(stream_url) + "/stt/offline",
        data=clip.read_bytes(),
        headers={
            "Content-Type": "application/octet-stream",
            "X-Audio-Filename": clip.name,
            "X-STT-Config-Overrides": json.dumps(
                offline_vad_overrides(session_config), separators=(",", ":")
            ),
        },
    )
    result["audio_sha256"] = audio_hash
    result["cache_key"] = cache_key
    write_json(cache_path, result)
    return result, cache_path, False


def normalize_text(text: str) -> str:
    normalized = unicodedata.normalize("NFKC", text).casefold()
    return " ".join(
        "".join(character if character.isalnum() else " " for character in normalized).split()
    )


def edit_distance(reference: list[str], hypothesis: list[str]) -> int:
    previous = list(range(len(hypothesis) + 1))
    for row, expected in enumerate(reference, 1):
        current = [row]
        for column, actual in enumerate(hypothesis, 1):
            current.append(
                min(
                    current[-1] + 1,
                    previous[column] + 1,
                    previous[column - 1] + (expected != actual),
                )
            )
        previous = current
    return previous[-1]


def error_rate(reference_text: str, hypothesis_text: str, unit: str) -> dict:
    reference_normalized = normalize_text(reference_text)
    hypothesis_normalized = normalize_text(hypothesis_text)
    if unit == "word":
        reference = reference_normalized.split()
        hypothesis = hypothesis_normalized.split()
    elif unit == "character":
        reference = list(reference_normalized.replace(" ", ""))
        hypothesis = list(hypothesis_normalized.replace(" ", ""))
    else:
        raise ValueError(f"unsupported error-rate unit: {unit}")
    edits = edit_distance(reference, hypothesis)
    if not reference:
        rate = 0.0 if not hypothesis else 1.0
    else:
        rate = edits / len(reference)
    return {
        "edits": edits,
        "reference_units": len(reference),
        "hypothesis_units": len(hypothesis),
        "rate": round(rate, 6),
    }


def percentile(values: list[float], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[round((len(ordered) - 1) * quantile)]


def distribution(values: list[float]) -> dict:
    if not values:
        return {"count": 0, "p50": None, "p90": None, "max": None, "mean": None}
    return {
        "count": len(values),
        "p50": round(percentile(values, 0.5), 3),
        "p90": round(percentile(values, 0.9), 3),
        "max": round(max(values), 3),
        "mean": round(sum(values) / len(values), 3),
    }


def event_values(events: list[dict], key: str) -> list[float]:
    return [
        float(event[key])
        for event in events
        if isinstance(event.get(key), (int, float)) and not isinstance(event.get(key), bool)
    ]


def assign_events(events: list[dict], clips: list[dict], audio_start_wall_sec: float) -> dict[str, list[dict]]:
    assigned = {clip["name"]: [] for clip in clips}
    for event in events:
        if not isinstance(event.get("start_sec"), (int, float)) or not isinstance(
            event.get("end_sec"), (int, float)
        ):
            continue
        event_start = float(event["start_sec"])
        event_end = float(event["end_sec"])
        best_name = None
        best_overlap = 0.0
        for clip in clips:
            clip_start = float(clip["play_start_wall_sec"]) - audio_start_wall_sec
            clip_end = float(clip["play_end_wall_sec"]) - audio_start_wall_sec
            overlap = max(0.0, min(event_end, clip_end) - max(event_start, clip_start))
            if overlap > best_overlap:
                best_overlap = overlap
                best_name = clip["name"]
        if best_name is not None:
            assigned[best_name].append(event)
    return assigned


def overlay_timeline(events: list[dict]) -> list[dict]:
    from scripts.subtitle_debug import SubtitleModel

    ordered = sorted(events, key=lambda event: event.get("seq", 0))
    model = SubtitleModel()
    records: list[dict] = []
    for event in ordered:
        received = event.get("bridge_received_wall_sec")
        if not isinstance(received, (int, float)):
            continue
        lines = model.apply(event.get("text", ""), bool(event.get("is_final")))
        records.append(
            {
                "seq": event.get("seq"),
                "is_final": bool(event.get("is_final")),
                "heard": event.get("text", ""),
                "lines": lines,
                "visible_from_wall_sec": float(received),
            }
        )
    for index, record in enumerate(records):
        next_time = (
            records[index + 1]["visible_from_wall_sec"]
            if index + 1 < len(records)
            else record["visible_from_wall_sec"] + CLEAR_TIMEOUT_SEC
        )
        record["visible_duration_sec"] = round(
            min(CLEAR_TIMEOUT_SEC, max(0.0, next_time - record["visible_from_wall_sec"])), 3
        )
    return records


def audio_metrics(path: Path, start_sec: float | None = None, end_sec: float | None = None) -> dict:
    if not path.exists():
        return {"available": False}
    with wave.open(str(path), "rb") as wav_file:
        channels = wav_file.getnchannels()
        sample_width = wav_file.getsampwidth()
        rate = wav_file.getframerate()
        total_frames = wav_file.getnframes()
        start_frame = max(0, int((start_sec or 0.0) * rate))
        end_frame = min(total_frames, int(end_sec * rate) if end_sec is not None else total_frames)
        wav_file.setpos(min(start_frame, total_frames))
        if sample_width != 2:
            return {"available": True, "sample_width_bits": sample_width * 8, "unsupported": True}
        frames_remaining = max(0, end_frame - start_frame)
        sample_count = 0
        sum_squares = 0
        peak = 0
        clipped = 0
        window_size = max(1, rate // 5)
        window_samples = 0
        window_squares = 0
        window_levels: list[float] = []
        while frames_remaining > 0:
            frame_count = min(frames_remaining, 4096)
            raw = wav_file.readframes(frame_count)
            if not raw:
                break
            values = struct.unpack(f"<{len(raw) // 2}h", raw)
            for index in range(0, len(values), channels):
                sample = values[index]
                square = sample * sample
                sample_count += 1
                sum_squares += square
                peak = max(peak, abs(sample))
                clipped += abs(sample) >= 32765
                window_samples += 1
                window_squares += square
                if window_samples >= window_size:
                    window_rms = math.sqrt(window_squares / window_samples)
                    window_levels.append(
                        -math.inf if window_rms <= 0 else 20.0 * math.log10(window_rms / 32768.0)
                    )
                    window_samples = 0
                    window_squares = 0
            frames_remaining -= frame_count
        if window_samples:
            window_rms = math.sqrt(window_squares / window_samples)
            window_levels.append(
                -math.inf if window_rms <= 0 else 20.0 * math.log10(window_rms / 32768.0)
            )
    if not sample_count:
        return {"available": True, "duration_sec": 0.0, "empty": True}
    rms = math.sqrt(sum_squares / sample_count)
    rms_dbfs = -math.inf if rms <= 0 else 20.0 * math.log10(rms / 32768.0)
    finite_windows = [value for value in window_levels if math.isfinite(value)]
    floor = percentile(finite_windows, 0.1)
    median = percentile(finite_windows, 0.5)
    loud = percentile(finite_windows, 0.9)
    dynamic_range = loud - floor if floor is not None and loud is not None else None
    clipped_percent = 100.0 * clipped / sample_count
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
        "duration_sec": round(sample_count / rate, 3),
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


def sequence_is_valid(events: list[dict]) -> bool:
    sequence = [event.get("seq") for event in events if isinstance(event.get("seq"), int)]
    return len(sequence) == len(set(sequence)) and all(a < b for a, b in zip(sequence, sequence[1:]))


def is_hallucination(text: str) -> bool:
    lowered = text.casefold()
    return any(marker in lowered for marker in HALLUCINATION_MARKERS)


def accuracy_result(reference: str, hypothesis: str, reference_kind: str) -> dict:
    wer = error_rate(reference, hypothesis, "word")
    cer = error_rate(reference, hypothesis, "character")
    return {
        "reference_kind": reference_kind,
        "wer": wer,
        "cer": cer,
        "score": round(max(0.0, 100.0 * (1.0 - wer["rate"])), 2),
    }


def aggregate_accuracy(pairs: list[tuple[str, str]], reference_kind: str) -> dict:
    word_results = [error_rate(reference, hypothesis, "word") for reference, hypothesis in pairs]
    character_results = [
        error_rate(reference, hypothesis, "character") for reference, hypothesis in pairs
    ]

    def aggregate(results: list[dict]) -> dict:
        edits = sum(result["edits"] for result in results)
        reference_units = sum(result["reference_units"] for result in results)
        hypothesis_units = sum(result["hypothesis_units"] for result in results)
        rate = edits / reference_units if reference_units else (0.0 if not hypothesis_units else 1.0)
        return {
            "edits": edits,
            "reference_units": reference_units,
            "hypothesis_units": hypothesis_units,
            "rate": round(rate, 6),
        }

    wer = aggregate(word_results)
    cer = aggregate(character_results)
    return {
        "reference_kind": reference_kind,
        "wer": wer,
        "cer": cer,
        "score": round(max(0.0, 100.0 * (1.0 - wer["rate"])), 2),
    }


def reliability_result(done: dict, valid_sequence: bool) -> dict:
    chunks = int(done.get("chunks_forwarded", 0) or 0)
    has_scoped_board_counter = "board_dropped_chunks_during_session" in done
    board_drops = int(
        done.get("board_dropped_chunks_during_session", done.get("board_dropped_chunks", 0)) or 0
    )
    board_drops_start = int(done.get("board_dropped_chunks_start", 0) or 0)
    board_drops_end = int(
        done.get("board_dropped_chunks_end", done.get("board_dropped_chunks", 0)) or 0
    )
    jobs = int(done.get("jobs_submitted", 0) or 0)
    partial_skips = int(done.get("partial_jobs_skipped", done.get("dropped_audio_jobs", 0)) or 0)
    # Old sessions did not distinguish disposable partials from final loss, so
    # retain their conservative score. New sessions explicitly report finals.
    final_drops = int(
        done.get(
            "final_jobs_dropped",
            done.get("dropped_audio_jobs", 0) if not has_scoped_board_counter else 0,
        )
        or 0
    )
    emitted = int(done.get("events_emitted", 0) or 0)
    event_drops = int(done.get("events_dropped", 0) or 0)
    transport = 100.0 * chunks / (chunks + board_drops) if chunks + board_drops else 0.0
    inference = 100.0 * max(0, jobs - final_drops) / jobs if jobs else 0.0
    delivery = 100.0 * (emitted - event_drops) / emitted if emitted else 0.0
    protocol_ok = (
        valid_sequence
        and done.get("status") == "complete"
        and done.get("event_queue_drained", True) is True
    )
    score = min(transport, inference, delivery) if protocol_ok else 0.0
    return {
        "score": round(max(0.0, score), 2),
        "protocol_ok": protocol_ok,
        "chunks_forwarded": chunks,
        "board_dropped_chunks_start": board_drops_start,
        "board_dropped_chunks_end": board_drops_end,
        "board_dropped_chunks_during_session": board_drops,
        "board_dropped_chunks": board_drops,
        "jobs_submitted": jobs,
        "partial_jobs_skipped": partial_skips,
        "final_jobs_dropped": final_drops,
        "dropped_audio_jobs": partial_skips,
        "events_emitted": emitted,
        "events_dropped": event_drops,
    }


def make_report(run_dir: Path, manifest: dict, ready: dict, done: dict) -> dict:
    events = load_jsonl(run_dir / "live" / "events.jsonl")
    audio_start_wall = float(ready["audio_start_wall_sec"])
    clips = manifest["clips"]
    assigned = assign_events(events, clips, audio_start_wall)
    overlay_records: dict[str, list[dict]] = {}
    clip_reports: list[dict] = []
    selected_references: list[str] = []
    live_hypotheses: list[str] = []
    offline_hypotheses: list[str] = []
    reference_kinds: list[str] = []

    for clip_index, clip in enumerate(clips):
        name = clip["name"]
        clip_events = assigned[name]
        finals = [event for event in clip_events if event.get("is_final")]
        live_text = " ".join(event.get("text", "").strip() for event in finals).strip()
        offline = load_json(run_dir / "offline" / f"{name}.json")
        offline_text = str(offline.get("text", ""))
        human_path = Path(clip["source_path"]).with_suffix(".txt")
        human_text = human_path.read_text(encoding="utf-8").strip() if human_path.exists() else None
        selected_reference = human_text if human_text is not None else offline_text
        reference_kind = "human" if human_text is not None else "offline_proxy"
        selected_references.append(selected_reference)
        live_hypotheses.append(live_text)
        offline_hypotheses.append(offline_text)
        reference_kinds.append(reference_kind)

        timeline = overlay_timeline(clip_events)
        overlay_records[name] = timeline
        visible_durations = [float(record["visible_duration_sec"]) for record in timeline]
        latencies = event_values(clip_events, "bridge_receive_lag_sec")
        latency_score = (
            100.0 * sum(value <= LATENCY_TARGET_SEC for value in latencies) / len(latencies)
            if latencies
            else 0.0
        )
        readability_score = (
            100.0
            * sum(value >= READABILITY_TARGET_SEC for value in visible_durations)
            / len(visible_durations)
            if visible_durations
            else 0.0
        )
        clip_start = float(clip["play_start_wall_sec"]) - audio_start_wall
        clip_end = float(clip["play_end_wall_sec"]) - audio_start_wall
        trace_end_wall = (
            float(clips[clip_index + 1]["play_start_wall_sec"])
            if clip_index + 1 < len(clips)
            else float(done.get("finished_wall_sec", clip["play_end_wall_sec"]))
        )
        partial_skips = counter_delta_for_interval(
            events,
            "partial_jobs_skipped",
            float(clip["play_start_wall_sec"]),
            trace_end_wall,
        )
        hallucination_candidates = sorted(
            {
                event.get("text", "").strip()
                for event in clip_events
                if is_hallucination(event.get("text", ""))
            }
        )
        receive_times = event_values(clip_events, "bridge_received_wall_sec")
        final_windows = [
            float(event["end_sec"]) - float(event["start_sec"])
            for event in finals
            if isinstance(event.get("start_sec"), (int, float))
            and isinstance(event.get("end_sec"), (int, float))
        ]
        max_window = ready.get("run_config", {}).get("config_max_window_sec")
        cap_hits = (
            sum(abs(window - float(max_window)) <= 0.06 for window in final_windows)
            if isinstance(max_window, (int, float))
            else None
        )
        clip_report = {
            "name": name,
            "source_path": clip["source_path"],
            "reference_kind": reference_kind,
            "human_reference_path": str(human_path) if human_text is not None else None,
            "offline_text": offline_text,
            "live_text": live_text,
            "accuracy": accuracy_result(selected_reference, live_text, reference_kind),
            "live_vs_offline": accuracy_result(offline_text, live_text, "offline_proxy"),
            "offline_vs_human": (
                accuracy_result(human_text, offline_text, "human") if human_text is not None else None
            ),
            "latency": distribution(latencies),
            "latency_score": round(latency_score, 2),
            "time_to_first_subtitle_sec": (
                round(min(receive_times) - float(clip["play_start_wall_sec"]), 3)
                if receive_times
                else None
            ),
            "readability": distribution(visible_durations),
            "readability_score": round(readability_score, 2),
            "update_rate_hz": (
                round((len(receive_times) - 1) / (max(receive_times) - min(receive_times)), 3)
                if len(receive_times) > 1 and max(receive_times) > min(receive_times)
                else None
            ),
            "events": len(clip_events),
            "finals": len(finals),
            "partials": len(clip_events) - len(finals),
            "partial_jobs_skipped": partial_skips,
            "final_jobs_dropped": 0,
            "hallucination_candidates": hallucination_candidates,
            "hallucinations": len(hallucination_candidates),
            "final_reasons": {
                reason: sum(event.get("segment_reason") == reason for event in finals)
                for reason in ("silence", "max_window", "flush")
            },
            "segmentation": {
                "final_window_sec": distribution(final_windows),
                "max_window_hits": cap_hits,
                "max_window_hit_percent": (
                    round(100.0 * cap_hits / len(final_windows), 2)
                    if cap_hits is not None and final_windows
                    else None
                ),
            },
            "vad": {
                "speech_ratio": distribution(event_values(clip_events, "vad_speech_ratio")),
                "trailing_silence_sec": distribution(
                    event_values(clip_events, "vad_trailing_silence_sec")
                ),
                "tail_rms_dbfs": distribution(event_values(clip_events, "tail_rms_dbfs")),
            },
            "pipeline": {
                "server_queue_sec": distribution(event_values(clip_events, "server_queue_sec")),
                "gpu_infer_sec": distribution(event_values(clip_events, "gpu_infer_sec")),
                "server_emit_lag_sec": distribution(
                    event_values(clip_events, "server_emit_lag_sec")
                ),
                "bridge_receive_lag_sec": distribution(latencies),
            },
            "audio": audio_metrics(run_dir / "live" / "board_audio.wav", clip_start, clip_end),
        }
        clip_reports.append(clip_report)
        (run_dir / "transcripts").mkdir(parents=True, exist_ok=True)
        (run_dir / "transcripts" / f"{name}-live.txt").write_text(live_text + "\n", encoding="utf-8")
        (run_dir / "transcripts" / f"{name}-offline.txt").write_text(
            offline_text + "\n", encoding="utf-8"
        )

    all_latencies = event_values(events, "bridge_receive_lag_sec")
    all_visible = [
        float(record["visible_duration_sec"])
        for records in overlay_records.values()
        for record in records
    ]
    reference_kind = (
        reference_kinds[0] if len(set(reference_kinds)) == 1 else "mixed_human_and_offline_proxy"
    )
    valid_sequence = sequence_is_valid(events)
    report = {
        "schema_version": 1,
        "run_id": manifest["run_id"],
        "status": "complete" if done.get("status") == "complete" else "invalid",
        "config": ready.get("run_config", {}),
        "scores": {
            "accuracy": aggregate_accuracy(
                list(zip(selected_references, live_hypotheses)), reference_kind
            ),
            "latency": round(
                100.0 * sum(value <= LATENCY_TARGET_SEC for value in all_latencies) / len(all_latencies),
                2,
            )
            if all_latencies
            else 0.0,
            "readability": round(
                100.0
                * sum(value >= READABILITY_TARGET_SEC for value in all_visible)
                / len(all_visible),
                2,
            )
            if all_visible
            else 0.0,
            "reliability": reliability_result(done, valid_sequence),
        },
        "global_metrics": {
            "latency": distribution(all_latencies),
            "visible_duration": distribution(all_visible),
            "events": len(events),
            "hallucination_candidates": sorted(
                {
                    event.get("text", "").strip()
                    for event in events
                    if is_hallucination(event.get("text", ""))
                }
            ),
            "sequence_valid": valid_sequence,
            "audio": audio_metrics(run_dir / "live" / "board_audio.wav"),
            "offline_vs_selected_reference": aggregate_accuracy(
                list(zip(selected_references, offline_hypotheses)), reference_kind
            ),
            "pipeline": {
                key: distribution(event_values(events, key))
                for key in (
                    "server_queue_sec",
                    "gpu_infer_sec",
                    "server_emit_lag_sec",
                    "bridge_receive_lag_sec",
                )
            },
        },
        "clips": clip_reports,
        "artifacts": {
            "manifest": "manifest.json",
            "events": "live/events.jsonl",
            "board_audio": "live/board_audio.wav",
            "overlay_timeline": "overlay_timeline.json",
        },
        "display_evidence": "reconstructed_from_events_not_physical_hdmi_capture",
    }
    write_json(run_dir / "overlay_timeline.json", overlay_records)
    write_json(run_dir / "report.json", report)
    (run_dir / "report.md").write_text(render_markdown(report), encoding="utf-8")
    return report


def fmt(value: object, digits: int = 2) -> str:
    return "n/a" if value is None else f"{float(value):.{digits}f}"


def render_markdown(report: dict) -> str:
    scores = report["scores"]
    accuracy = scores["accuracy"]
    reliability = scores["reliability"]
    lines = [
        f"# STT short-audio test — {report['run_id']}",
        "",
        "> El overlay es una reconstrucción de los eventos enviados; no es una captura HDMI física.",
        "",
        "| Categoría | Puntaje |",
        "|---|---:|",
        f"| Exactitud ({accuracy['reference_kind']}) | {fmt(accuracy['score'])} |",
        f"| Latencia ≤ {LATENCY_TARGET_SEC:.1f} s | {fmt(scores['latency'])} |",
        f"| Legibilidad ≥ {READABILITY_TARGET_SEC:.1f} s | {fmt(scores['readability'])} |",
        f"| Confiabilidad | {fmt(reliability['score'])} |",
        "",
        "| Audio | Referencia | WER live | CER live | p90 latencia | Partial skips |",
        "|---|---|---:|---:|---:|---:|",
    ]
    for clip in report["clips"]:
        lines.append(
            f"| {clip['name']} | {clip['reference_kind']} | "
            f"{fmt(100.0 * clip['accuracy']['wer']['rate'])}% | "
            f"{fmt(100.0 * clip['accuracy']['cer']['rate'])}% | "
            f"{fmt(clip['latency']['p90'])} s | {clip['partial_jobs_skipped']} |"
        )
    lines.extend(
        [
            "",
            "## Métricas globales",
            "",
            f"- WER: {fmt(100.0 * accuracy['wer']['rate'])}%",
            f"- CER: {fmt(100.0 * accuracy['cer']['rate'])}%",
            f"- Latencia p90: {fmt(report['global_metrics']['latency']['p90'])} s",
            f"- Eventos: {report['global_metrics']['events']}",
            f"- Secuencia válida: {report['global_metrics']['sequence_valid']}",
            f"- Parciales omitidos por backpressure: {reliability['partial_jobs_skipped']}",
            f"- Finales perdidos: {reliability['final_jobs_dropped']}",
            f"- Chunks perdidos durante el test: {reliability['board_dropped_chunks_during_session']}",
            f"- Contador de drops previo al test: {reliability['board_dropped_chunks_start']}",
            f"- Candidatos a alucinación: {len(report['global_metrics']['hallucination_candidates'])}",
            "",
        ]
    )
    return "\n".join(lines)


def print_summary(report: dict) -> None:
    scores = report["scores"]
    accuracy = scores["accuracy"]
    reliability = scores["reliability"]
    print("\nSTT SHORT-AUDIO RESULT")
    print(f"  run          : {report['run_id']}")
    print(f"  accuracy     : {accuracy['score']:.2f}/100 ({accuracy['reference_kind']})")
    print(f"  latency      : {scores['latency']:.2f}/100 <= {LATENCY_TARGET_SEC:.1f}s")
    print(f"  readability  : {scores['readability']:.2f}/100 >= {READABILITY_TARGET_SEC:.1f}s")
    print(f"  reliability  : {reliability['score']:.2f}/100")
    print(f"  WER / CER    : {100.0 * accuracy['wer']['rate']:.2f}% / {100.0 * accuracy['cer']['rate']:.2f}%")
    print(f"  latency p90  : {fmt(report['global_metrics']['latency']['p90'])}s")
    print(f"  partial skips: {reliability['partial_jobs_skipped']} (not final losses)")
    print(f"  real drops   : {reliability['board_dropped_chunks_during_session']} audio / {reliability['final_jobs_dropped']} finals")
    print(f"  report       : logs/audio-tests/{report['run_id']}/report.md")


def compare_runs() -> int:
    reports = []
    for path in sorted(LOG_ROOT.glob("*/report.json")):
        try:
            reports.append(load_json(path))
        except (OSError, ValueError, json.JSONDecodeError):
            continue
    if not reports:
        print("No completed audio-test reports found.")
        return 0
    header = (
        f"{'RUN':<24} {'MODEL':<20} {'WIN':>5} {'SIL':>5} {'PART':>5} {'AGR':>4} {'VTH':>5} {'VNEG':>5} "
        f"{'ACC':>7} {'LAT':>7} {'READ':>7} {'REL':>7} {'WER%':>7} {'P90':>7} "
        f"{'PSKIP':>6} {'LOSS':>5}"
    )
    print(header)
    print("-" * len(header))
    for report in reports:
        config = report.get("config", {})
        scores = report.get("scores", {})
        accuracy = scores.get("accuracy", {})
        reliability = scores.get("reliability", {})
        latency = report.get("global_metrics", {}).get("latency", {})
        model = Path(str(config.get("config_model", "?"))).name
        print(
            f"{report.get('run_id', '?'):<24} {model:<20} "
            f"{fmt(config.get('config_max_window_sec'), 1):>5} "
            f"{fmt(config.get('config_min_silence_sec'), 1):>5} "
            f"{fmt(config.get('config_partial_sec'), 1):>5} "
            f"{str(config.get('config_partial_agreement', '?')):>4} "
            f"{fmt(config.get('config_vad_threshold'), 2):>5} "
            f"{fmt(config.get('config_vad_neg_threshold'), 2):>5} "
            f"{fmt(accuracy.get('score')):>7} {fmt(scores.get('latency')):>7} "
            f"{fmt(scores.get('readability')):>7} {fmt(reliability.get('score')):>7} "
            f"{fmt(100.0 * accuracy.get('wer', {}).get('rate', 0.0)):>7} "
            f"{fmt(latency.get('p90')):>7} "
            f"{str(reliability.get('partial_jobs_skipped', reliability.get('dropped_audio_jobs', '?'))):>6} "
            f"{str(reliability.get('final_jobs_dropped', '?')):>5}"
        )
    return 0


def wait_for_file(path: Path, process: subprocess.Popen, timeout_sec: float) -> dict:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        if path.exists():
            return load_json(path)
        if process.poll() is not None:
            raise RuntimeError(f"bridge exited with code {process.returncode} before creating {path.name}")
        time.sleep(0.2)
    raise TimeoutError(f"timed out after {timeout_sec:.0f}s waiting for {path.name}")


def new_run_dir() -> Path:
    base = datetime.now().strftime("%Y%m%d-%H%M%S")
    candidate = LOG_ROOT / base
    suffix = 1
    while candidate.exists():
        candidate = LOG_ROOT / f"{base}-{suffix}"
        suffix += 1
    candidate.mkdir(parents=True)
    return candidate


def run_offline(
    clips: list[Path],
    stream_url: str,
    health: dict,
    refresh: bool,
    output_dir: Path | None,
    session_config: dict | None = None,
) -> list[dict]:
    references = []
    for clip in clips:
        print(f"Offline reference: {clip.name} ...", flush=True)
        reference, cache_path, cache_hit = offline_reference(
            clip, stream_url, health, refresh, session_config
        )
        print(f"  {'cache hit' if cache_hit else 'generated'}: {cache_path.relative_to(REPO_ROOT)}")
        references.append(reference)
        if output_dir is not None:
            write_json(output_dir / f"{clip.stem}.json", reference)
    return references


def run_benchmark(args) -> int:
    audio_dir = Path(os.environ.get("AUDIO_TEST_DIR", str(DEFAULT_AUDIO_DIR)))
    stream_url = os.environ.get("STT_STREAM_URL", DEFAULT_STREAM_URL)
    clips = discover_clips(audio_dir)
    health = health_check(stream_url)
    requested_config = requested_session_config(args)
    if args.offline_only:
        run_offline(clips, stream_url, health, args.refresh_offline, None, requested_config)
        return 0
    if shutil.which("ffplay") is None:
        raise RuntimeError("ffplay is required but was not found in WSL PATH")

    gap_sec = float(os.environ.get("AUDIO_TEST_GAP_SEC", "6"))
    if gap_sec <= CLEAR_TIMEOUT_SEC:
        raise RuntimeError(f"AUDIO_TEST_GAP_SEC must be greater than {CLEAR_TIMEOUT_SEC:.1f}s")
    ready_timeout = float(os.environ.get("AUDIO_TEST_READY_TIMEOUT_SEC", "90"))
    run_dir = new_run_dir()
    live_dir = run_dir / "live"
    offline_dir = run_dir / "offline"
    live_dir.mkdir()
    offline_dir.mkdir()
    ready_file = run_dir / "bridge-ready.json"
    done_file = run_dir / "bridge-done.json"
    stop_file = run_dir / "bridge-stop"
    manifest = {
        "schema_version": 1,
        "run_id": run_dir.name,
        "status": "starting",
        "created_wall_sec": time.time(),
        "audio_dir": str(audio_dir),
        "stream_url": stream_url,
        "gap_sec": gap_sec,
        "health": health,
        "requested_session_config": requested_config,
        "sweep": getattr(args, "sweep_metadata", None),
        "clips": [],
    }
    write_json(run_dir / "manifest.json", manifest)
    try:
        run_offline(
            clips, stream_url, health, args.refresh_offline, offline_dir, requested_config
        )
    except Exception:
        manifest["status"] = "failed_offline"
        manifest["finished_wall_sec"] = time.time()
        write_json(run_dir / "manifest.json", manifest)
        raise

    relative = lambda path: str(path.relative_to(REPO_ROOT)).replace(os.sep, "/")
    environment = os.environ.copy()
    environment.update(
        {
            "STT_STREAM_URL": stream_url,
            "STT_FOREGROUND": "1",
            "STT_SINGLE_SESSION": "1",
            "STT_JSONL": relative(live_dir / "events.jsonl"),
            "STT_SAVE_WAV": relative(live_dir / "board_audio.wav"),
            "STT_READY_FILE": relative(ready_file),
            "STT_DONE_FILE": relative(done_file),
            "STT_STOP_FILE": relative(stop_file),
        }
    )
    for name, value in requested_config.items():
        _config_key, environment_key = SESSION_PARAMETER_MAP[name]
        environment[environment_key] = str(value)
    bridge_log = (live_dir / "bridge.log").open("w", encoding="utf-8")
    bridge = subprocess.Popen(
        [str(REPO_ROOT / "scripts" / "run_stt_colab_stream.sh")],
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
        raise KeyboardInterrupt

    previous_sigint = signal.signal(signal.SIGINT, request_stop)
    previous_sigterm = signal.signal(signal.SIGTERM, request_stop)
    try:
        print(
            f"Waiting up to {ready_timeout:.0f}s for board audio on TCP port "
            f"{environment.get('STT_AUDIO_PORT', '5000')} ...",
            flush=True,
        )
        ready = wait_for_file(ready_file, bridge, ready_timeout)
        verify_effective_config(requested_config, ready.get("run_config", {}))
        manifest["status"] = "playing"
        manifest["bridge_ready"] = ready
        write_json(run_dir / "manifest.json", manifest)
        print(f"Bridge ready. Initial silence: {gap_sec:.1f}s", flush=True)
        time.sleep(gap_sec)
        for clip in clips:
            if interrupted:
                raise KeyboardInterrupt
            print(f"Playing {clip.name}", flush=True)
            clip_record = {
                "name": clip.stem,
                "source_path": str(clip),
                "source_sha256": sha256_file(clip),
                "play_start_wall_sec": time.time(),
            }
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
                    str(clip),
                ],
                check=False,
            )
            clip_record["play_end_wall_sec"] = time.time()
            clip_record["player_exit_code"] = result.returncode
            manifest["clips"].append(clip_record)
            write_json(run_dir / "manifest.json", manifest)
            if result.returncode != 0:
                raise RuntimeError(f"ffplay failed for {clip.name} with code {result.returncode}")
            print(f"Silence between clips: {gap_sec:.1f}s", flush=True)
            time.sleep(gap_sec)
        stop_file.touch()
        try:
            bridge.wait(timeout=45)
        except subprocess.TimeoutExpired as exc:
            bridge.terminate()
            raise RuntimeError("bridge did not stop within 45 seconds") from exc
        if bridge.returncode != 0:
            raise RuntimeError(f"bridge exited with code {bridge.returncode}; inspect {live_dir / 'bridge.log'}")
        done = wait_for_file(done_file, bridge, 2)
        manifest["status"] = "complete"
        manifest["finished_wall_sec"] = time.time()
        manifest["bridge_done"] = done
        write_json(run_dir / "manifest.json", manifest)
        report = make_report(run_dir, manifest, ready, done)
        args.completed_report = report
        print_summary(report)
        return 0
    except KeyboardInterrupt:
        manifest["status"] = "interrupted"
        manifest["finished_wall_sec"] = time.time()
        write_json(run_dir / "manifest.json", manifest)
        print(f"\nInterrupted; partial artifacts kept in {run_dir.relative_to(REPO_ROOT)}", file=sys.stderr)
        return 130
    except Exception:
        manifest["status"] = "failed"
        manifest["finished_wall_sec"] = time.time()
        write_json(run_dir / "manifest.json", manifest)
        raise
    finally:
        stop_file.touch(exist_ok=True)
        if bridge.poll() is None:
            try:
                bridge.wait(timeout=5)
            except subprocess.TimeoutExpired:
                bridge.terminate()
                try:
                    bridge.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    bridge.kill()
        bridge_log.close()
        signal.signal(signal.SIGINT, previous_sigint)
        signal.signal(signal.SIGTERM, previous_sigterm)


def load_sweep_cases(path: Path | None = None) -> list[dict]:
    if path is None:
        raw_cases = [dict(case) for case in DEFAULT_SWEEP_CASES]
    else:
        parsed = json.loads(path.read_text(encoding="utf-8"))
        raw_cases = parsed.get("cases") if isinstance(parsed, dict) else parsed
        if not isinstance(raw_cases, list):
            raise ValueError("sweep file must be a JSON list or an object with a 'cases' list")
    if not raw_cases:
        raise ValueError("sweep must contain at least one case")

    cases = []
    names = set()
    for index, raw_case in enumerate(raw_cases, 1):
        if not isinstance(raw_case, dict):
            raise ValueError(f"sweep case {index} must be a JSON object")
        unknown = sorted(set(raw_case) - ({"name"} | set(SESSION_PARAMETER_MAP)))
        if unknown:
            raise ValueError(f"unsupported field(s) in sweep case {index}: {', '.join(unknown)}")
        name = str(raw_case.get("name", f"case_{index}")).strip()
        if not name or name in names:
            raise ValueError(f"sweep case name must be non-empty and unique: {name!r}")
        config = validate_config_overrides(
            {key: value for key, value in raw_case.items() if key != "name"}
        )
        if not config:
            raise ValueError(f"sweep case {name!r} has no session parameters")
        names.add(name)
        cases.append({"name": name, **config})
    return cases


def new_sweep_dir() -> Path:
    root = LOG_ROOT / "sweeps"
    base = datetime.now().strftime("%Y%m%d-%H%M%S")
    candidate = root / base
    suffix = 1
    while candidate.exists():
        candidate = root / f"{base}-{suffix}"
        suffix += 1
    candidate.mkdir(parents=True)
    return candidate


def sweep_row(case: dict, report: dict) -> dict:
    scores = report["scores"]
    reliability = scores["reliability"]
    return {
        "name": case["name"],
        "requested_config": {key: value for key, value in case.items() if key != "name"},
        "run_id": report["run_id"],
        "status": report["status"],
        "accuracy": scores["accuracy"]["score"],
        "latency": scores["latency"],
        "readability": scores["readability"],
        "reliability": reliability["score"],
        "wer_percent": round(100.0 * scores["accuracy"]["wer"]["rate"], 2),
        "cer_percent": round(100.0 * scores["accuracy"]["cer"]["rate"], 2),
        "latency_p90_sec": report["global_metrics"]["latency"]["p90"],
        "partial_jobs_skipped": reliability["partial_jobs_skipped"],
        "real_audio_drops": reliability["board_dropped_chunks_during_session"],
        "final_jobs_dropped": reliability["final_jobs_dropped"],
        "hallucination_candidates": len(report["global_metrics"]["hallucination_candidates"]),
    }


def render_sweep_markdown(sweep: dict) -> str:
    lines = [
        f"# STT parameter sweep — {sweep['sweep_id']}",
        "",
        "No se calcula un promedio global: exactitud, latencia, legibilidad y confiabilidad se comparan por separado.",
        "",
        "| Caso | Win | Silence | Partial | Agr | VAD th | VAD neg | Accuracy | Latency | Readability | Reliability | WER | p90 | P skips | Real loss | Halluc. |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in sweep["runs"]:
        if row.get("status") == "error":
            lines.append(f"| {row['name']} | colspan=13: ERROR — {row['error']} |")
            continue
        config = row["requested_config"]
        lines.append(
            f"| [{row['name']}](../../{row['run_id']}/report.md) | "
            f"{fmt(config.get('max_window_sec'), 1)} | "
            f"{fmt(config.get('min_silence_sec'), 1)} | "
            f"{fmt(config.get('partial_sec'), 1)} | "
            f"{config.get('partial_agreement', 'n/a')} | "
            f"{fmt(config.get('vad_threshold'), 2)} | "
            f"{fmt(config.get('vad_neg_threshold'), 2)} | "
            f"{fmt(row['accuracy'])} | {fmt(row['latency'])} | "
            f"{fmt(row['readability'])} | {fmt(row['reliability'])} | "
            f"{fmt(row['wer_percent'])}% | {fmt(row['latency_p90_sec'])} s | "
            f"{row['partial_jobs_skipped']} | "
            f"{row['real_audio_drops'] + row['final_jobs_dropped']} | "
            f"{row['hallucination_candidates']} |"
        )
    if sweep.get("best_by_metric"):
        lines.extend(["", "## Mejores casos por métrica", ""])
        for metric, names in sweep["best_by_metric"].items():
            lines.append(f"- {metric}: {', '.join(names)}")
    return "\n".join(lines) + "\n"


def run_sweep(args) -> int:
    cases = load_sweep_cases(Path(args.sweep_file) if args.sweep_file else None)
    sweep_dir = new_sweep_dir()
    sweep = {
        "schema_version": 1,
        "sweep_id": sweep_dir.name,
        "created_wall_sec": time.time(),
        "status": "running",
        "cases": cases,
        "runs": [],
    }
    write_json(sweep_dir / "manifest.json", sweep)
    print(f"\nSTT PARAMETER SWEEP: {len(cases)} sequential runs")
    print(f"Aggregate report: {sweep_dir.relative_to(REPO_ROOT) / 'report.md'}")

    exit_code = 0
    for index, case in enumerate(cases, 1):
        config = {key: value for key, value in case.items() if key != "name"}
        print(f"\n=== SWEEP {index}/{len(cases)}: {case['name']} ===")
        print("  " + "  ".join(f"{key}={value}" for key, value in config.items()))
        run_args = copy.copy(args)
        run_args.offline_only = False
        run_args.compare = False
        run_args.completed_report = None
        run_args.sweep_metadata = {
            "sweep_id": sweep["sweep_id"],
            "case_index": index,
            "case_name": case["name"],
        }
        for parameter in SESSION_PARAMETER_MAP:
            setattr(run_args, parameter, config.get(parameter))
        try:
            result = run_benchmark(run_args)
            if result == 130:
                raise KeyboardInterrupt
            if result != 0 or run_args.completed_report is None:
                raise RuntimeError(f"benchmark returned {result} without a report")
            sweep["runs"].append(sweep_row(case, run_args.completed_report))
        except KeyboardInterrupt:
            sweep["status"] = "interrupted"
            exit_code = 130
            break
        except Exception as exc:
            exit_code = 1
            sweep["runs"].append({"name": case["name"], "status": "error", "error": str(exc)})
            print(f"Sweep case {case['name']} failed: {exc}", file=sys.stderr)
        finally:
            write_json(sweep_dir / "manifest.json", sweep)
        if index < len(cases):
            time.sleep(float(os.environ.get("AUDIO_TEST_SWEEP_GAP_SEC", "3")))

    successful = [row for row in sweep["runs"] if row.get("status") != "error"]
    best_by_metric = {}
    for metric in ("accuracy", "latency", "readability", "reliability"):
        if successful:
            best = max(row[metric] for row in successful)
            best_by_metric[metric] = [row["name"] for row in successful if row[metric] == best]
    sweep["best_by_metric"] = best_by_metric
    sweep["status"] = sweep["status"] if sweep["status"] == "interrupted" else (
        "complete" if exit_code == 0 else "complete_with_errors"
    )
    sweep["finished_wall_sec"] = time.time()
    write_json(sweep_dir / "manifest.json", sweep)
    write_json(sweep_dir / "report.json", sweep)
    (sweep_dir / "report.md").write_text(render_sweep_markdown(sweep), encoding="utf-8")
    print(f"\nSweep finished: {sweep['status']}")
    print(f"Report: {sweep_dir.relative_to(REPO_ROOT) / 'report.md'}")
    return exit_code


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--offline-only", action="store_true", help="only populate offline cache")
    mode.add_argument("--compare", action="store_true", help="compare completed historical runs")
    mode.add_argument("--sweep", action="store_true", help="run the built-in parameter matrix")
    mode.add_argument("--sweep-file", help="run cases from a JSON parameter matrix")
    parser.add_argument("--max-window-sec", type=float)
    parser.add_argument("--min-silence-sec", type=float)
    parser.add_argument("--partial-sec", type=float)
    parser.add_argument("--partial-agreement", type=int)
    parser.add_argument("--gain", type=float)
    parser.add_argument("--vad-threshold", type=float)
    parser.add_argument("--vad-neg-threshold", type=float)
    parser.add_argument(
        "--refresh-offline", action="store_true", help="ignore and replace matching offline cache entries"
    )
    args = parser.parse_args()
    try:
        requested_session_config(args)
    except ValueError as exc:
        parser.error(str(exc))
    return args


def main() -> int:
    args = parse_args()
    if args.compare:
        return compare_runs()
    try:
        if args.sweep or args.sweep_file:
            return run_sweep(args)
        return run_benchmark(args)
    except (OSError, RuntimeError, ValueError, TimeoutError) as exc:
        print(f"audio test failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
