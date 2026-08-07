#!/usr/bin/env python3
"""One-shot MediaSpeech ES evaluation for the selected Nemotron pipeline.

This is intentionally separate from ``audio_test_short.py`` and every physical
audio sweep.  It runs two immutable phases with one already-selected operating
point:

1. complete-file transcription against the human MediaSpeech references;
2. accelerated cache-aware streaming through the production
   :class:`scripts.stt_nemotron_backend.NemotronSession`, without real-time
   sleeps or a test-only inference implementation.

Results are append-only JSONL files on Drive.  A rerun resumes at the first clip
without a successful record, while an identity mismatch aborts instead of
mixing models, source revisions or parameters in one evaluation directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import statistics
import sys
import time
import traceback
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Iterable, Sequence


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.stt_dataset_manifest import (
    ARCHIVE_SHA256,
    DATASET_ID,
    DATASET_VERSION,
    DatasetClip,
    manifest_fingerprint,
    normalize_spanish_text,
)
from scripts.stt_nemotron_backend import (
    MODEL_ID,
    NEMO_COMMIT,
    RUN_ENGINE,
    TARGET_RATE,
    NemotronConfig,
    transcribe_offline_float32,
)


SCHEMA_VERSION = 1
EVALUATION_ID = "mediaspeech-es-v1.1__nemotron-560-600-2__v1"
REFERENCE_KIND = "human_mediaspeech_double_annotated"
CHECKPOINT_EVERY = 25


class EvaluationIdentityError(RuntimeError):
    """An output directory belongs to a different immutable evaluation."""


class EvaluationIncompleteError(RuntimeError):
    """A phase contains failed or missing clips and cannot feed the next one."""


@dataclass(frozen=True)
class SequenceError:
    edits: int
    reference_units: int
    hypothesis_units: int

    @property
    def rate(self) -> float:
        if self.reference_units == 0:
            return 0.0 if self.hypothesis_units == 0 else 1.0
        return self.edits / self.reference_units

    def as_dict(self) -> dict:
        return {
            "edits": self.edits,
            "reference_units": self.reference_units,
            "hypothesis_units": self.hypothesis_units,
            "rate": round(self.rate, 8),
        }


def fixed_nemotron_config() -> NemotronConfig:
    """The selected operating point; this final evaluation exposes no tuning knobs."""
    return NemotronConfig(
        model_id=MODEL_ID,
        target_lang="es-ES",
        latency_ms=560,
        decoder_type="rnnt",
        stop_history_eou_ms=600,
        residue_tokens_at_end=2,
        strip_lang_tags=True,
        asr_output_granularity="segment",
        compute_dtype="float32",
        use_amp=True,
        device="cuda",
        device_id=0,
    )


def validate_fixed_config(config: NemotronConfig) -> None:
    expected = fixed_nemotron_config().as_effective_config()
    actual = config.as_effective_config()
    if actual != expected:
        raise EvaluationIdentityError(
            "the MediaSpeech final evaluation is fixed at 560/600/2 es-ES; "
            f"received {json.dumps(actual, sort_keys=True)}"
        )


def edit_distance(reference: Sequence[str], hypothesis: Sequence[str]) -> int:
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


def error_rate(reference_text: str, hypothesis_text: str, *, unit: str) -> SequenceError:
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
    return SequenceError(
        edits=edit_distance(reference, hypothesis),
        reference_units=len(reference),
        hypothesis_units=len(hypothesis),
    )


def percentile(values: Sequence[float], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(float(value) for value in values)
    index = max(0, min(len(ordered) - 1, round((len(ordered) - 1) * quantile)))
    return ordered[index]


def distribution(values: Iterable[float], *, digits: int = 6) -> dict:
    clean = [float(value) for value in values if math.isfinite(float(value))]
    if not clean:
        return {
            "count": 0,
            "mean": None,
            "p50": None,
            "p90": None,
            "p95": None,
            "p99": None,
            "min": None,
            "max": None,
        }
    return {
        "count": len(clean),
        "mean": round(statistics.fmean(clean), digits),
        "p50": round(percentile(clean, 0.50), digits),
        "p90": round(percentile(clean, 0.90), digits),
        "p95": round(percentile(clean, 0.95), digits),
        "p99": round(percentile(clean, 0.99), digits),
        "min": round(min(clean), digits),
        "max": round(max(clean), digits),
    }


def load_flac_float32(path: Path):
    """Load the corpus' native 16 kHz mono FLAC without transcoding."""
    try:
        import soundfile as sf
    except ImportError as exc:
        raise RuntimeError("soundfile is required to read MediaSpeech FLAC files") from exc

    audio, sample_rate = sf.read(str(path), dtype="float32", always_2d=True)
    if int(sample_rate) != TARGET_RATE:
        raise ValueError(f"unexpected sample rate for {path}: {sample_rate} != {TARGET_RATE}")
    if audio.shape[1] != 1:
        raise ValueError(f"expected mono audio for {path}, found {audio.shape[1]} channels")
    return audio[:, 0]


def consolidate_streaming_text(events: Iterable[dict]) -> str:
    """Reconstruct the committed transcript, excluding replaceable partials."""
    finals = [
        str(event.get("full_text") or event.get("text") or "").strip()
        for event in events
        if event.get("is_final")
    ]
    return " ".join(piece for piece in finals if piece).strip()


def _json_fingerprint(value: dict) -> str:
    encoded = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _write_json_atomic(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    temporary.replace(path)


def _read_jsonl_latest(path: Path, *, fingerprint: str | None = None) -> dict[str, dict]:
    """Read append-only results; tolerate only a torn final line after interruption."""
    latest: dict[str, dict] = {}
    for record in _read_jsonl_all(path, fingerprint=fingerprint):
        clip_id = record.get("clip_id")
        if isinstance(clip_id, str):
            latest[clip_id] = record
    return latest


def _read_jsonl_all(path: Path, *, fingerprint: str | None = None) -> list[dict]:
    """Read every durable attempt, including failures later recovered by resume."""
    records: list[dict] = []
    if not path.is_file():
        return records
    lines = path.read_text(encoding="utf-8").splitlines()
    for index, line in enumerate(lines):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            if index == len(lines) - 1:
                break
            raise EvaluationIdentityError(f"corrupt JSONL record at {path}:{index + 1}") from None
        if fingerprint and record.get("evaluation_fingerprint") != fingerprint:
            raise EvaluationIdentityError(
                f"result identity mismatch at {path}:{index + 1}; use a new output directory"
            )
        records.append(record)
    return records


def build_evaluation_identity(
    clips: Sequence[DatasetClip],
    config: NemotronConfig,
    *,
    project_commit: str,
    model_provenance: dict,
) -> dict:
    validate_fixed_config(config)
    locked = {
        "schema_version": SCHEMA_VERSION,
        "evaluation_id": EVALUATION_ID,
        "dataset_id": DATASET_ID,
        "dataset_version": DATASET_VERSION,
        "dataset_archive_sha256": ARCHIVE_SHA256,
        "dataset_manifest_sha256": manifest_fingerprint(clips),
        "expected_clips": len(clips),
        "run_engine": RUN_ENGINE,
        "nemo_commit": NEMO_COMMIT,
        "project_commit": str(project_commit),
        "model_revision": model_provenance.get("model_revision"),
        "chunk_size_in_secs": model_provenance.get("chunk_size_in_secs"),
        # RTF is hardware/runtime dependent. Lock the complete plain-data
        # provenance so a resume on a different Colab GPU cannot silently mix
        # throughput measurements under one report.
        "model_provenance": model_provenance,
        "config": config.as_effective_config(),
        "reference_kind": REFERENCE_KIND,
        "streaming_mode": "accelerated_production_session_no_sleep",
    }
    return {**locked, "fingerprint": _json_fingerprint(locked)}


class EvaluationStore:
    """Append-only phase results plus small atomic status files."""

    def __init__(self, output_dir: Path, identity: dict):
        self.output_dir = Path(output_dir).resolve()
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.identity = identity
        self.fingerprint = identity["fingerprint"]
        identity_path = self.output_dir / "evaluation.json"
        if identity_path.is_file():
            existing = json.loads(identity_path.read_text(encoding="utf-8"))
            if existing.get("fingerprint") != self.fingerprint:
                raise EvaluationIdentityError(
                    f"{self.output_dir} belongs to another evaluation; do not mix results"
                )
        else:
            _write_json_atomic(
                identity_path,
                {**identity, "created_at": _utc_now()},
            )

    def results_path(self, phase: str) -> Path:
        return self.output_dir / f"{phase}_results.jsonl"

    def latest(self, phase: str) -> dict[str, dict]:
        return _read_jsonl_latest(self.results_path(phase), fingerprint=self.fingerprint)

    def write_progress(self, phase: str, payload: dict) -> None:
        _write_json_atomic(
            self.output_dir / f"{phase}_progress.json",
            {
                "schema_version": SCHEMA_VERSION,
                "evaluation_fingerprint": self.fingerprint,
                "phase": phase,
                "updated_at": _utc_now(),
                **payload,
            },
        )


class NemotronDatasetEvaluator:
    """Offline then accelerated-streaming evaluation using one loaded model."""

    def __init__(
        self,
        shared_model,
        dataset_root: Path,
        clips: Sequence[DatasetClip],
        output_dir: Path,
        *,
        project_commit: str,
        config: NemotronConfig | None = None,
        audio_loader: Callable[[Path], object] = load_flac_float32,
        offline_transcriber: Callable = transcribe_offline_float32,
        checkpoint_every: int = CHECKPOINT_EVERY,
    ):
        self.shared_model = shared_model
        self.dataset_root = Path(dataset_root).resolve()
        self.clips = list(clips)
        self.config = config or fixed_nemotron_config()
        validate_fixed_config(self.config)
        if not self.clips:
            raise ValueError("evaluation needs at least one clip")
        self.audio_loader = audio_loader
        self.offline_transcriber = offline_transcriber
        self.checkpoint_every = max(1, int(checkpoint_every))
        self.provenance = shared_model.provenance()
        self.identity = build_evaluation_identity(
            self.clips,
            self.config,
            project_commit=project_commit,
            model_provenance=self.provenance,
        )
        self.store = EvaluationStore(output_dir, self.identity)
        _write_json_atomic(self.store.output_dir / "model_provenance.json", self.provenance)

    def _common_record(self, phase: str, clip: DatasetClip, audio, decode_sec: float) -> dict:
        duration = len(audio) / float(TARGET_RATE)
        return {
            "schema_version": SCHEMA_VERSION,
            "evaluation_fingerprint": self.store.fingerprint,
            "phase": phase,
            "clip_id": clip.clip_id,
            "audio_relpath": clip.audio_relpath,
            "audio_sha256": clip.audio_sha256,
            "reference_relpath": clip.reference_relpath,
            "reference_sha256": clip.reference_sha256,
            "reference_kind": REFERENCE_KIND,
            "reference_raw": clip.reference_raw,
            "reference_normalized": clip.reference_normalized,
            "reference_words": clip.reference_words,
            "audio_duration_sec": round(duration, 6),
            "speech_rate_reference_wps": round(clip.reference_words / duration, 6),
            "decode_sec": round(decode_sec, 6),
        }

    def _offline_clip(self, clip: DatasetClip) -> dict:
        started = time.monotonic()
        audio = self.audio_loader(clip.audio_path(self.dataset_root))
        decode_sec = time.monotonic() - started
        record = self._common_record("offline", clip, audio, decode_sec)
        inference_started = time.monotonic()
        result = self.offline_transcriber(self.shared_model, audio, self.config)
        inference_sec = time.monotonic() - inference_started
        text = str(result.get("text", ""))
        duration = float(record["audio_duration_sec"])
        record.update(
            {
                "status": "ok",
                "hypothesis_raw": text,
                "hypothesis_normalized": normalize_spanish_text(text),
                "segments": result.get("segments") or [],
                "wer_vs_human": error_rate(clip.reference_raw, text, unit="word").as_dict(),
                "cer_vs_human": error_rate(clip.reference_raw, text, unit="character").as_dict(),
                "inference_sec": round(inference_sec, 6),
                "inference_rtf": round(inference_sec / duration, 8) if duration else None,
                "total_wall_sec": round(time.monotonic() - started, 6),
                "completed_at": _utc_now(),
            }
        )
        return record

    def _streaming_clip(self, clip: DatasetClip, offline: dict) -> dict:
        started = time.monotonic()
        audio = self.audio_loader(clip.audio_path(self.dataset_root))
        decode_sec = time.monotonic() - started
        record = self._common_record("streaming", clip, audio, decode_sec)
        session = self.shared_model.build_session(self.config, source_rate=TARGET_RATE)
        events: list[dict] = []
        inference_started = time.monotonic()
        try:
            events.extend(session.push_float32(audio))
            events.extend(session.flush())
            stats = session.stats_snapshot()
        finally:
            session.close()
        inference_sec = time.monotonic() - inference_started
        text = consolidate_streaming_text(events)
        duration = float(record["audio_duration_sec"])
        event_end_times = [
            float(event["end_sec"])
            for event in events
            if isinstance(event.get("end_sec"), (int, float))
        ]
        final_reasons: dict[str, int] = {}
        for event in events:
            reason = event.get("final_reason")
            if event.get("is_final") and isinstance(reason, str):
                final_reasons[reason] = final_reasons.get(reason, 0) + 1
        record.update(
            {
                "status": "ok",
                "mode": "accelerated_production_session_no_sleep",
                "hypothesis_raw": text,
                "hypothesis_normalized": normalize_spanish_text(text),
                "offline_hypothesis_raw": offline["hypothesis_raw"],
                "wer_vs_human": error_rate(clip.reference_raw, text, unit="word").as_dict(),
                "cer_vs_human": error_rate(clip.reference_raw, text, unit="character").as_dict(),
                "wer_vs_offline": error_rate(
                    offline["hypothesis_raw"], text, unit="word"
                ).as_dict(),
                "cer_vs_offline": error_rate(
                    offline["hypothesis_raw"], text, unit="character"
                ).as_dict(),
                "human_wer_delta_vs_offline": round(
                    error_rate(clip.reference_raw, text, unit="word").rate
                    - float(offline["wer_vs_human"]["rate"]),
                    8,
                ),
                "events": events,
                "event_count": len(events),
                "final_reasons": final_reasons,
                "first_subtitle_audio_sec": round(min(event_end_times), 6) if event_end_times else None,
                "session_stats": stats,
                "inference_sec": round(inference_sec, 6),
                "inference_rtf": round(inference_sec / duration, 8) if duration else None,
                "total_wall_sec": round(time.monotonic() - started, 6),
                "completed_at": _utc_now(),
            }
        )
        return record

    def _run_phase(self, phase: str, worker: Callable[[DatasetClip], dict]) -> dict:
        path = self.store.results_path(phase)
        latest = self.store.latest(phase)
        completed = {clip_id for clip_id, row in latest.items() if row.get("status") == "ok"}
        pending = [clip for clip in self.clips if clip.clip_id not in completed]
        print(f"{phase}: {len(completed)}/{len(self.clips)} complete; {len(pending)} pending")

        path.parent.mkdir(parents=True, exist_ok=True)
        processed_this_run = 0
        errors_this_run = 0
        handle = path.open("a", encoding="utf-8")
        try:
            for clip in pending:
                try:
                    record = worker(clip)
                except KeyboardInterrupt:
                    raise
                except Exception as exc:  # noqa: BLE001 - persist per-clip failures and continue
                    errors_this_run += 1
                    record = {
                        "schema_version": SCHEMA_VERSION,
                        "evaluation_fingerprint": self.store.fingerprint,
                        "phase": phase,
                        "clip_id": clip.clip_id,
                        "audio_relpath": clip.audio_relpath,
                        "audio_sha256": clip.audio_sha256,
                        "reference_sha256": clip.reference_sha256,
                        "status": "error",
                        "error_type": type(exc).__name__,
                        "error": str(exc)[:1000],
                        "traceback": "".join(
                            traceback.format_exception(type(exc), exc, exc.__traceback__)
                        )[-4000:],
                        "completed_at": _utc_now(),
                    }
                handle.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")
                processed_this_run += 1
                latest[clip.clip_id] = record
                if processed_this_run % self.checkpoint_every == 0:
                    handle.flush()
                    os.fsync(handle.fileno())
                    ok = sum(row.get("status") == "ok" for row in latest.values())
                    self.store.write_progress(
                        phase,
                        {
                            "status": "running",
                            "completed_clips": ok,
                            "total_clips": len(self.clips),
                            "errors_latest": sum(
                                row.get("status") == "error" for row in latest.values()
                            ),
                        },
                    )
                    print(f"  {phase}: {ok}/{len(self.clips)} checkpointed")
        finally:
            handle.flush()
            os.fsync(handle.fileno())
            handle.close()

        latest = self.store.latest(phase)
        ok = sum(row.get("status") == "ok" for row in latest.values())
        errors = sum(row.get("status") == "error" for row in latest.values())
        status = "complete" if ok == len(self.clips) and errors == 0 else "incomplete"
        progress = {
            "status": status,
            "completed_clips": ok,
            "total_clips": len(self.clips),
            "errors_latest": errors,
            "processed_this_run": processed_this_run,
            "errors_this_run": errors_this_run,
        }
        self.store.write_progress(phase, progress)
        self.write_reports()
        return progress

    def run_offline(self) -> dict:
        return self._run_phase("offline", self._offline_clip)

    def run_streaming(self) -> dict:
        offline = self.store.latest("offline")
        missing = [
            clip.clip_id
            for clip in self.clips
            if offline.get(clip.clip_id, {}).get("status") != "ok"
        ]
        if missing:
            raise EvaluationIncompleteError(
                f"offline must finish before streaming; {len(missing)} clips are missing/failed"
            )
        self.shared_model.configure_streaming(self.config)
        return self._run_phase(
            "streaming", lambda clip: self._streaming_clip(clip, offline[clip.clip_id])
        )

    def run_all(self) -> dict:
        offline = self.run_offline()
        if offline["status"] != "complete":
            raise EvaluationIncompleteError(
                "offline phase finished with errors; rerun the notebook to retry them"
            )
        streaming = self.run_streaming()
        if streaming["status"] != "complete":
            raise EvaluationIncompleteError(
                "streaming phase finished with errors; rerun the notebook to retry them"
            )
        return self.write_reports()

    def write_reports(self) -> dict:
        offline = self.store.latest("offline")
        streaming = self.store.latest("streaming")
        offline_attempts = _read_jsonl_all(
            self.store.results_path("offline"), fingerprint=self.store.fingerprint
        )
        streaming_attempts = _read_jsonl_all(
            self.store.results_path("streaming"), fingerprint=self.store.fingerprint
        )
        summary = build_summary(
            self.identity,
            list(offline.values()),
            list(streaming.values()),
            offline_attempts=offline_attempts,
            streaming_attempts=streaming_attempts,
        )
        _write_json_atomic(self.store.output_dir / "summary.json", summary)
        (self.store.output_dir / "report.md").write_text(
            render_report(summary), encoding="utf-8"
        )
        errors = [row for row in offline_attempts + streaming_attempts if row.get("status") == "error"]
        errors_path = self.store.output_dir / "errors.jsonl"
        errors_path.write_text(
            "".join(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n" for row in errors),
            encoding="utf-8",
        )
        return summary


def _micro_error(rows: Sequence[dict], key: str) -> dict:
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


def _wer_strata(rows: Sequence[dict], value_key: str, boundaries: Sequence[float]) -> list[dict]:
    """Micro/macro human WER in fixed, predeclared numeric strata."""
    edges = [float("-inf"), *[float(value) for value in boundaries], float("inf")]
    strata: list[dict] = []
    for lower, upper in zip(edges, edges[1:]):
        selected = [
            row
            for row in rows
            if isinstance(row.get(value_key), (int, float))
            and lower <= float(row[value_key]) < upper
        ]
        label = (
            f"<{upper:g}" if math.isinf(lower)
            else f">={lower:g}" if math.isinf(upper)
            else f"{lower:g}..<{upper:g}"
        )
        strata.append(
            {
                "range": label,
                "clips": len(selected),
                "micro_wer_vs_human": _micro_error(selected, "wer_vs_human")
                if selected
                else None,
                "macro_wer_vs_human": distribution(
                    row["wer_vs_human"]["rate"] for row in selected
                ),
            }
        )
    return strata


def _phase_summary(records: Sequence[dict], expected: int) -> dict:
    ok = [row for row in records if row.get("status") == "ok"]
    errors = [row for row in records if row.get("status") == "error"]
    summary = {
        "status": "complete" if len(ok) == expected and not errors else "incomplete",
        "completed_clips": len(ok),
        "expected_clips": expected,
        "failed_clips": len(errors),
        "micro_wer_vs_human": _micro_error(ok, "wer_vs_human") if ok else None,
        "micro_cer_vs_human": _micro_error(ok, "cer_vs_human") if ok else None,
        "macro_wer_vs_human": distribution(row["wer_vs_human"]["rate"] for row in ok),
        "macro_cer_vs_human": distribution(row["cer_vs_human"]["rate"] for row in ok),
        "inference_rtf": distribution(
            row["inference_rtf"] for row in ok if row.get("inference_rtf") is not None
        ),
        "inference_sec_total": round(sum(float(row["inference_sec"]) for row in ok), 3),
        "audio_hours": round(
            sum(float(row["audio_duration_sec"]) for row in ok) / 3600.0, 6
        ),
        "perfect_word_clips": sum(row["wer_vs_human"]["edits"] == 0 for row in ok),
        "wer_by_duration_sec": _wer_strata(ok, "audio_duration_sec", (10.0, 14.0)),
        "wer_by_reference_speech_rate_wps": _wer_strata(
            ok, "speech_rate_reference_wps", (2.3, 2.9)
        ),
    }
    if ok:
        summary["perfect_word_clip_percent"] = round(
            100.0 * summary["perfect_word_clips"] / len(ok), 4
        )
        worst = sorted(ok, key=lambda row: row["wer_vs_human"]["rate"], reverse=True)[:20]
        summary["worst_wer_clips"] = [
            {
                "clip_id": row["clip_id"],
                "wer": row["wer_vs_human"]["rate"],
                "reference": row["reference_raw"],
                "hypothesis": row["hypothesis_raw"],
            }
            for row in worst
        ]
    return summary


def build_summary(
    identity: dict,
    offline_records: Sequence[dict],
    streaming_records: Sequence[dict],
    *,
    offline_attempts: Sequence[dict] | None = None,
    streaming_attempts: Sequence[dict] | None = None,
) -> dict:
    expected = int(identity["expected_clips"])
    offline = _phase_summary(offline_records, expected)
    streaming = _phase_summary(streaming_records, expected)
    streaming_ok = [row for row in streaming_records if row.get("status") == "ok"]
    if streaming_ok:
        streaming.update(
            {
                "micro_wer_vs_offline": _micro_error(streaming_ok, "wer_vs_offline"),
                "micro_cer_vs_offline": _micro_error(streaming_ok, "cer_vs_offline"),
                "macro_wer_vs_offline": distribution(
                    row["wer_vs_offline"]["rate"] for row in streaming_ok
                ),
                "human_wer_delta_vs_offline": distribution(
                    row["human_wer_delta_vs_offline"] for row in streaming_ok
                ),
                "first_subtitle_audio_sec": distribution(
                    row["first_subtitle_audio_sec"]
                    for row in streaming_ok
                    if row.get("first_subtitle_audio_sec") is not None
                ),
                "events_total": sum(int(row["event_count"]) for row in streaming_ok),
                "model_eou_total": sum(
                    int(row["final_reasons"].get("model_eou", 0)) for row in streaming_ok
                ),
                "session_flush_total": sum(
                    int(row["final_reasons"].get("session_flush", 0)) for row in streaming_ok
                ),
                "display_rollup_total": sum(
                    int(row["final_reasons"].get("display_rollup", 0)) for row in streaming_ok
                ),
                "partial_revisions_total": sum(
                    int(row["session_stats"].get("partial_revisions", 0))
                    for row in streaming_ok
                ),
            }
        )
    overall_status = (
        "complete" if offline["status"] == streaming["status"] == "complete" else "incomplete"
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "generated_at": _utc_now(),
        "status": overall_status,
        "identity": identity,
        "measurement_scope": {
            "offline": "complete-file model quality against human references",
            "streaming": "accelerated production NemotronSession; no real-time sleep",
            "latency_warning": (
                "Accelerated streaming measures model audio-clock emission points and RTF, "
                "not board-to-HDMI wall-clock latency."
            ),
            "file_boundary_warning": (
                "MediaSpeech clips often end mid-sentence; session_flush is an artificial "
                "file boundary and is not counted as a natural model EOU."
            ),
            "physical_replay": "deferred to phase 3 and absent from this report",
        },
        "execution_history": {
            "offline_attempt_records": len(offline_attempts or offline_records),
            "streaming_attempt_records": len(streaming_attempts or streaming_records),
            "offline_retried_records": max(
                0, len(offline_attempts or offline_records) - len(offline_records)
            ),
            "streaming_retried_records": max(
                0, len(streaming_attempts or streaming_records) - len(streaming_records)
            ),
            "historical_errors": sum(
                row.get("status") == "error"
                for row in list(offline_attempts or offline_records)
                + list(streaming_attempts or streaming_records)
            ),
        },
        "offline": offline,
        "streaming": streaming,
    }


def _percent(rate: float | None) -> str:
    return "n/a" if rate is None else f"{100.0 * float(rate):.2f}%"


def render_report(summary: dict) -> str:
    identity = summary["identity"]
    offline = summary["offline"]
    streaming = summary["streaming"]
    config = identity["config"]
    lines = [
        "# Evaluación final Nemotron sobre MediaSpeech ES",
        "",
        f"Estado: **{summary['status']}**",
        "",
        "## Identidad congelada",
        "",
        f"- Dataset: `{identity['dataset_id']} {identity['dataset_version']}` "
        f"({identity['expected_clips']} clips; referencias humanas)",
        f"- Archive SHA-256: `{identity['dataset_archive_sha256']}`",
        f"- Modelo: `{config['model_id']}`",
        f"- Motor: `{identity['run_engine']}` / NeMo `{identity['nemo_commit']}`",
        f"- Idioma: `{config['target_lang']}`",
        f"- Streaming: lookahead `{config['latency_ms']} ms`, contexto "
        f"`{config['att_context_size']}`, EOU `{config['stop_history_eou_ms']} ms`, "
        f"residuo `{config['residue_tokens_at_end']}`",
        f"- Commit del proyecto: `{identity['project_commit']}`",
        "",
        "## Resultados",
        "",
        "| Fase | Estado | Clips | WER micro humano | CER micro humano | RTF p50 | RTF p90 |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for name, phase in (("Offline", offline), ("Streaming acelerado", streaming)):
        wer = phase.get("micro_wer_vs_human") or {}
        cer = phase.get("micro_cer_vs_human") or {}
        rtf = phase.get("inference_rtf") or {}
        lines.append(
            f"| {name} | {phase['status']} | {phase['completed_clips']}/{phase['expected_clips']} "
            f"| {_percent(wer.get('rate'))} | {_percent(cer.get('rate'))} "
            f"| {rtf.get('p50', 'n/a')} | {rtf.get('p90', 'n/a')} |"
        )
    lines.extend(
        [
            "",
            "## Degradación streaming",
            "",
            f"- WER micro streaming contra offline: "
            f"{_percent((streaming.get('micro_wer_vs_offline') or {}).get('rate'))}",
            f"- Delta macro WER humano streaming-offline (media): "
            f"{_percent((streaming.get('human_wer_delta_vs_offline') or {}).get('mean'))}",
            f"- Eventos: {streaming.get('events_total', 0)}; model EOU: "
            f"{streaming.get('model_eou_total', 0)}; display rollups: "
            f"{streaming.get('display_rollup_total', 0)}; session flushes: "
            f"{streaming.get('session_flush_total', 0)}",
            "",
            "## Alcance de lo medido",
            "",
            "La fase offline mide la capacidad del modelo sobre el archivo completo. La fase "
            "streaming usa la misma `NemotronSession` cache-aware de producción, pero alimenta "
            "los frames sin dormir en tiempo real.",
            "",
            "> Esta corrida **no** mide todavía latencia física placa→HDMI. El tiempo de primera "
            "aparición se expresa en el reloj del audio del modelo y el RTF mide cómputo. El replay "
            "físico de una hora queda reservado para la fase 3.",
            "",
            "> El final de cada FLAC es una frontera artificial. `session_flush` no se interpreta "
            "como EOU acústico; sólo `model_eou` representa una decisión del endpointer.",
            "",
            "## Artefactos",
            "",
            "- `evaluation.json`: identidad y fingerprint inmutables",
            "- `manifest.jsonl`: pares audio/referencia y hashes",
            "- `offline_results.jsonl`: resultados por clip de fase 1",
            "- `streaming_results.jsonl`: resultados/eventos por clip de fase 2",
            "- `*_progress.json`: checkpoints reanudables",
            "- `summary.json`, `report.md`, `errors.jsonl`: agregados finales",
            "",
        ]
    )
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Inspect an existing fixed Nemotron MediaSpeech evaluation"
    )
    parser.add_argument("output_dir", type=Path, help="Drive evaluation output directory")
    args = parser.parse_args(argv)
    summary_path = args.output_dir / "summary.json"
    if not summary_path.is_file():
        parser.error(f"summary not found: {summary_path}; run the Colab evaluation notebook")
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    print(render_report(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
