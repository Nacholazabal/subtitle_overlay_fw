#!/usr/bin/env python3
"""Spanish FLEURS evaluation through the production Nemotron inference path.

The Hugging Face dataset is supplied by the caller (normally only
``google/fleurs`` / ``es_419`` / ``test``). This module does not start an API
server, download data itself, or alter the separate MediaSpeech evaluation.
"""

from __future__ import annotations

import hashlib
import json
import io
import os
import time
import traceback
from pathlib import Path
from typing import Callable, Iterable

import numpy as np

from server.evaluation.dataset import (
    EvaluationStore,
    _json_fingerprint,
    _utc_now,
    _write_json_atomic,
    consolidate_streaming_text,
    fixed_nemotron_config,
)
from server.evaluation.wer_normalization import WER_PROFILES, normalized_error_rate
from server.runtime.nemotron import (
    TARGET_RATE,
    NemotronConfig,
    transcribe_offline_float32,
)


SCHEMA_VERSION = 1
DATASET_ID = "google/fleurs"
DATASET_CONFIG = "es_419"
DATASET_SPLIT = "test"
REFERENCE_KIND = "crowd_sourced_fleurs_raw_transcription"
CHECKPOINT_EVERY = 25


def _format_duration(seconds: float | None) -> str:
    """Format an elapsed/remaining duration for compact notebook progress."""
    if seconds is None or not np.isfinite(seconds):
        return "unknown"
    rounded = max(0, int(round(seconds)))
    hours, remainder = divmod(rounded, 3600)
    minutes, secs = divmod(remainder, 60)
    if hours:
        return f"{hours:d}h {minutes:02d}m"
    if minutes:
        return f"{minutes:d}m {secs:02d}s"
    return f"{secs:d}s"


def _audio_array(row: dict) -> np.ndarray:
    audio = row["audio"]
    if isinstance(audio, dict):
        samples = audio.get("array")
        rate = audio.get("sampling_rate")
        if samples is None:
            try:
                import soundfile as sf
            except ImportError as exc:
                raise RuntimeError("soundfile is required to decode FLEURS audio") from exc
            if audio.get("bytes"):
                samples, rate = sf.read(io.BytesIO(audio["bytes"]), dtype="float32", always_2d=True)
            elif audio.get("path"):
                samples, rate = sf.read(str(audio["path"]), dtype="float32", always_2d=True)
            else:
                decoder = audio.get("decoder") or audio.get("audio")
                if decoder is not None and hasattr(decoder, "get_all_samples"):
                    decoded = decoder.get_all_samples()
                    samples = decoded.data.cpu().numpy().T
                    rate = decoded.sample_rate
                else:
                    raise ValueError("FLEURS audio has neither decoded samples nor bytes/path")
    else:
        samples, rate = audio, row.get("sampling_rate")
    if samples is None:
        raise ValueError("FLEURS row has no decoded audio")
    if rate is None:
        raise ValueError("FLEURS audio row does not declare its sampling rate")
    if int(rate) != TARGET_RATE:
        raise ValueError(f"FLEURS audio must be {TARGET_RATE} Hz, found {rate}")
    result = np.asarray(samples, dtype="float32")
    if result.ndim == 2:
        if result.shape[1] != 1:
            raise ValueError(f"FLEURS audio must be mono, found {result.shape[1]} channels")
        result = result[:, 0]
    if result.ndim != 1:
        raise ValueError(f"FLEURS audio must be mono, found shape {result.shape}")
    if result.size == 0:
        raise ValueError("FLEURS audio is empty")
    return result


def _reference(row: dict) -> str:
    # raw_transcription preserves punctuation/casing for transparent scoring;
    # fallback supports older datasets that expose only transcription.
    value = row.get("raw_transcription") or row.get("transcription")
    if not isinstance(value, str) or not value.strip():
        raise ValueError("FLEURS row has no non-empty transcription")
    return value.strip()


def _row_id(row: dict) -> str:
    sentence_id = row.get("id")
    if sentence_id is None:
        raise ValueError("FLEURS row has no sentence id")
    # FLEURS repeats a sentence id for different people reading that sentence.
    # Its recording filename, not the sentence id alone, identifies a clip.
    path = row.get("path")
    if not path and isinstance(row.get("audio"), dict):
        path = row["audio"].get("path")
    if not isinstance(path, str) or not path:
        raise ValueError(f"FLEURS sentence {sentence_id} has no recording path")
    return f"{sentence_id}:{Path(path).name}"


def _audio_sha(audio: np.ndarray) -> str:
    return hashlib.sha256(np.asarray(audio, dtype="<f4").tobytes()).hexdigest()


def _metric(reference: str, hypothesis: str) -> dict:
    return {
        profile: normalized_error_rate(
            reference, hypothesis, unit="word", profile=profile
        ).as_dict()
        for profile in WER_PROFILES
    }


def fixed_fleurs_config() -> NemotronConfig:
    """Selected 560/600/2 es-ES operating point used by MediaSpeech."""
    return fixed_nemotron_config()


class FleursEvaluator:
    """Resumable FLEURS run over one shared model and its production sessions.

    ``rows_factory`` must return a fresh iterator over the same immutable test
    split each time. Dataset fingerprints and recording IDs are locked before
    inference, preventing partial output from being mixed across sources.
    """

    def __init__(
        self,
        shared_model,
        rows_factory: Callable[[], Iterable[dict]],
        output_dir: Path,
        *,
        project_commit: str,
        dataset_fingerprint: str,
        config: NemotronConfig | None = None,
        checkpoint_every: int = CHECKPOINT_EVERY,
        offline_transcriber: Callable = transcribe_offline_float32,
    ):
        self.shared_model = shared_model
        self.rows_factory = rows_factory
        self.output_dir = Path(output_dir)
        self.config = config or fixed_fleurs_config()
        if self.config.as_effective_config() != fixed_fleurs_config().as_effective_config():
            raise ValueError("FLEURS comparison is fixed to the selected 560/600/2 es-ES config")
        self.checkpoint_every = max(1, int(checkpoint_every))
        self.offline_transcriber = offline_transcriber
        row_digest = hashlib.sha256()
        ids: list[str] = []
        seen: set[str] = set()
        for row in self.rows_factory():
            clip_id, reference = _row_id(row), _reference(row)
            if clip_id in seen:
                raise ValueError(f"duplicate FLEURS recording: {clip_id}")
            seen.add(clip_id)
            ids.append(clip_id)
            row_digest.update(clip_id.encode("utf-8") + b"\0")
            row_digest.update(hashlib.sha256(reference.encode("utf-8")).hexdigest().encode("ascii") + b"\n")
        if not ids:
            raise ValueError("FLEURS split is empty")
        self.provenance = shared_model.provenance()
        locked = {
            "schema_version": SCHEMA_VERSION,
            "evaluation_id": "fleurs-es_419-test__nemotron-560-600-2__v1",
            "dataset_id": DATASET_ID,
            "dataset_config": DATASET_CONFIG,
            "dataset_split": DATASET_SPLIT,
            "dataset_fingerprint": str(dataset_fingerprint),
            "reference_kind": REFERENCE_KIND,
            "manifest_sha256": row_digest.hexdigest(),
            "clip_ids": ids,
            "expected_clips": len(ids),
            "run_engine": self.provenance.get("run_engine"),
            "nemo_commit": self.provenance.get("nemo_commit"),
            "project_commit": str(project_commit),
            "model_provenance": self.provenance,
            "config": self.config.as_effective_config(),
            "streaming_mode": "accelerated_production_session_no_sleep",
            "wer_profiles": list(WER_PROFILES),
            "comparison_note": "Not an exact reproduction of NVIDIA's published WER; its exact normalizer is not published.",
        }
        self.identity = {**locked, "fingerprint": _json_fingerprint(locked)}
        self.store = EvaluationStore(self.output_dir, self.identity)
        _write_json_atomic(self.output_dir / "model_provenance.json", self.provenance)

    def _run_phase(self, phase: str) -> dict:
        path = self.store.results_path(phase)
        latest = self.store.latest(phase)
        total = self.identity["expected_clips"]
        completed_at_start = sum(
            row.get("status") == "ok" for row in latest.values()
        )
        print(
            f"{phase}: {completed_at_start}/{total} complete; "
            "resuming by stable clip id",
            flush=True,
        )
        processed = 0
        errors_this_run = 0
        phase_started = time.monotonic()
        with path.open("a", encoding="utf-8") as handle:
            for row in self.rows_factory():
                clip_id = _row_id(row)
                if latest.get(clip_id, {}).get("status") == "ok":
                    continue
                reference = _reference(row)
                try:
                    started = time.monotonic()
                    audio = _audio_array(row)
                    decode_sec = time.monotonic() - started
                    inference_started = time.monotonic()
                    if phase == "offline":
                        result = self.offline_transcriber(self.shared_model, audio, self.config)
                        text = str(result.get("text", ""))
                        extra = {"segments": result.get("segments") or []}
                    else:
                        session = self.shared_model.build_session(self.config, source_rate=TARGET_RATE)
                        events = []
                        try:
                            events.extend(session.push_float32(audio))
                            events.extend(session.flush())
                            stats = session.stats_snapshot()
                        finally:
                            session.close()
                        text = consolidate_streaming_text(events)
                        extra = {"events": events, "session_stats": stats, "event_count": len(events)}
                    inference_sec = time.monotonic() - inference_started
                    duration = audio.size / float(TARGET_RATE)
                    record = {
                        "schema_version": SCHEMA_VERSION,
                        "evaluation_fingerprint": self.store.fingerprint,
                        "phase": phase,
                        "clip_id": clip_id,
                        "sentence_id": row["id"],
                        "recording_path": row.get("path"),
                        "speaker_id": row.get("speaker_id"),
                        "audio_sha256_float32_16khz": _audio_sha(audio),
                        "reference_kind": REFERENCE_KIND,
                        "reference_raw": reference,
                        "hypothesis_raw": text,
                        "wer": _metric(reference, text),
                        "audio_duration_sec": round(duration, 6),
                        "decode_sec": round(decode_sec, 6),
                        "inference_sec": round(inference_sec, 6),
                        "inference_rtf": round(inference_sec / duration, 8) if duration else None,
                        "mode": "offline_complete_file" if phase == "offline" else "accelerated_production_session_no_sleep",
                        "status": "ok",
                        "completed_at": _utc_now(),
                        **extra,
                    }
                except KeyboardInterrupt:
                    raise
                except Exception as exc:  # persist individual failure; retry on resume
                    errors_this_run += 1
                    record = {
                        "schema_version": SCHEMA_VERSION,
                        "evaluation_fingerprint": self.store.fingerprint,
                        "phase": phase,
                        "clip_id": clip_id,
                        "reference_raw": reference,
                        "status": "error",
                        "error_type": type(exc).__name__,
                        "error": str(exc)[:1000],
                        "traceback": "".join(traceback.format_exception(exc))[-3000:],
                        "completed_at": _utc_now(),
                    }
                handle.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")
                processed += 1
                latest[clip_id] = record
                if processed % self.checkpoint_every == 0:
                    handle.flush()
                    os.fsync(handle.fileno())
                    completed = sum(
                        row.get("status") == "ok" for row in latest.values()
                    )
                    elapsed = time.monotonic() - phase_started
                    clips_per_minute = processed * 60.0 / elapsed if elapsed else 0.0
                    remaining_attempts = max(
                        total - completed_at_start - processed, 0
                    )
                    eta_sec = (
                        remaining_attempts * elapsed / processed
                        if processed
                        else None
                    )
                    self.store.write_progress(
                        phase,
                        {
                            "status": "running",
                            "completed_clips": completed,
                            "total_clips": total,
                            "processed_this_run": processed,
                            "errors_this_run": errors_this_run,
                            "elapsed_sec": round(elapsed, 3),
                            "clips_per_minute": round(clips_per_minute, 3),
                            "eta_sec": round(eta_sec, 3) if eta_sec is not None else None,
                        },
                    )
                    print(
                        f"{phase}: {completed}/{total} complete; "
                        f"{errors_this_run} errors; "
                        f"{clips_per_minute:.1f} clips/min; "
                        f"ETA {_format_duration(eta_sec)}",
                        flush=True,
                    )
            handle.flush()
            os.fsync(handle.fileno())
        latest = self.store.latest(phase)
        ok = [r for r in latest.values() if r.get("status") == "ok"]
        status = "complete" if len(ok) == self.identity["expected_clips"] else "incomplete"
        elapsed = time.monotonic() - phase_started
        progress = {"status": status, "completed_clips": len(ok), "total_clips": self.identity["expected_clips"], "errors_latest": sum(r.get("status") == "error" for r in latest.values()), "processed_this_run": processed, "errors_this_run": errors_this_run, "elapsed_sec": round(elapsed, 3)}
        self.store.write_progress(phase, progress)
        self.write_reports()
        print(
            f"{phase}: {status}; {len(ok)}/{total} complete; "
            f"{errors_this_run} errors this run; elapsed {_format_duration(elapsed)}",
            flush=True,
        )
        return progress

    def run_offline(self) -> dict:
        return self._run_phase("offline")

    def run_streaming(self) -> dict:
        if len([r for r in self.store.latest("offline").values() if r.get("status") == "ok"]) != self.identity["expected_clips"]:
            raise RuntimeError("offline FLEURS phase must complete before streaming")
        self.shared_model.configure_streaming(self.config)
        return self._run_phase("streaming")

    def run_all(self) -> dict:
        if self.run_offline()["status"] != "complete":
            raise RuntimeError("FLEURS offline phase incomplete; rerun to retry failures")
        if self.run_streaming()["status"] != "complete":
            raise RuntimeError("FLEURS streaming phase incomplete; rerun to retry failures")
        return self.write_reports()

    def write_reports(self) -> dict:
        phases = {}
        for phase in ("offline", "streaming"):
            rows = list(self.store.latest(phase).values())
            good = [r for r in rows if r.get("status") == "ok"]
            metrics = {}
            for profile in WER_PROFILES:
                edits = sum(r["wer"][profile]["edits"] for r in good)
                refs = sum(r["wer"][profile]["reference_units"] for r in good)
                metrics[profile] = {"edits": edits, "reference_units": refs, "rate": edits / refs if refs else None, "percent": 100 * edits / refs if refs else None}
            phases[phase] = {"status": "complete" if len(good) == self.identity["expected_clips"] else "incomplete", "completed_clips": len(good), "expected_clips": self.identity["expected_clips"], "wer": metrics}
        summary = {"schema_version": SCHEMA_VERSION, "generated_at": _utc_now(), "status": "complete" if all(v["status"] == "complete" for v in phases.values()) else "incomplete", "identity": self.identity, "source_provenance": {"dataset": DATASET_ID, "config": DATASET_CONFIG, "split": DATASET_SPLIT, "dataset_fingerprint": self.identity["dataset_fingerprint"], "reference_field": "raw_transcription (fallback: transcription)", "audio_representation_hash": "decoded float32 mono samples at 16 kHz"}, "measurement_scope": {"streaming": "accelerated production NemotronSession; no real-time sleep", "published_benchmark_scope": "NVIDIA's 4.26% is reported for 560 ms streaming FLEURS Spanish with language ID; only this runner's streaming phase is in the comparable operating mode. Offline is a distinct complete-file path.", "latency_warning": "Not physical board-to-HDMI latency.", "benchmark_warning": "This is not claimed to reproduce 4.26%: NVIDIA's exact text normalizer is not published. FLEURS is read speech; keep MediaSpeech results separate."}, "phases": phases}
        _write_json_atomic(self.output_dir / "summary.json", summary)
        lines = ["# Nemotron en FLEURS español (es_419/test)", "", f"Estado: **{summary['status']}**", "", "FLEURS es habla leída y este resultado no reemplaza ni modifica MediaSpeech.", "El 4,26% publicado por NVIDIA corresponde a streaming de 560 ms con language ID; solo nuestra fase streaming es comparable en modo de operación. La fase offline es una medición distinta. No afirmamos reproducir exactamente el benchmark: la normalización textual de NVIDIA no está publicada completa.", "", "| Fase | Estado | clips | WER legacy | WER numeric_es |", "|---|---|---:|---:|---:|"]
        for phase, data in phases.items():
            vals = data["wer"]
            fmt = lambda key: "n/a" if vals[key]["percent"] is None else f"{vals[key]['percent']:.2f}%"
            lines.append(f"| {phase} | {data['status']} | {data['completed_clips']}/{data['expected_clips']} | {fmt('legacy')} | {fmt('numeric_es')} |")
        lines.extend(["", "`legacy` follows the previous project normalizer; `numeric_es` is the project's explicit Spanish number-normalization profile. Neither is asserted to be NVIDIA's unpublished normalizer.", "", "Artefactos: `evaluation.json`, `model_provenance.json`, `offline_results.jsonl`, `streaming_results.jsonl`, progress JSON, `summary.json` y este informe.", ""])
        (self.output_dir / "report.md").write_text("\n".join(lines), encoding="utf-8")
        return summary
