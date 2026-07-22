#!/usr/bin/env python3
"""SimulStreaming / AlignAtt STT backend adapter.

This module wraps the official upstream project
``https://github.com/ufal/SimulStreaming`` (PyTorch Whisper + AlignAtt + VAC) so
that it can drive the *existing* subtitle overlay pipeline (board -> bridge ->
WebSocket -> firmware ACK -> HDMI overlay) as a drop-in alternative to the
faster-whisper streaming engine.

Design rules that make this file importable and testable everywhere:

* **No heavy imports at module load.** ``torch``/``numpy``/SimulStreaming are
  imported lazily inside the functions that need them, so this module imports
  cleanly in the WSL test environment (no GPU, no torch). Only the actual model
  load / inference paths pull those dependencies, and those only run in Colab.
* **The online processor is injected.** :class:`SimulStreamingSession` and
  :class:`TranscriptAdapter` operate on any object exposing
  ``insert_audio_chunk`` / ``process_iter`` / ``finish`` (the upstream
  ``VACOnlineASRProcessor`` / ``SimulWhisperOnline`` contract). Unit tests pass a
  fake, so the entire result-mapping logic runs without SimulStreaming present.

We deliberately do **not** wrap this inside the faster-whisper ``ChunkTranscriber``:
SimulStreaming owns its own incremental processing, VAC endpointing, AlignAtt
commit decisions and last-word truncation. Mixing the two segmentation stacks
would make it impossible to tell which algorithm produced a result.
"""

from __future__ import annotations

import hashlib
import time
from dataclasses import dataclass, asdict, replace
from pathlib import Path


# --- Provenance / pinned experiment identity -------------------------------

RUN_ENGINE = "simulstreaming_alignatt"
# Full upstream commit this integration is pinned to. Resolved with
# ``git ls-remote https://github.com/ufal/SimulStreaming HEAD`` during
# implementation. The Colab notebook checks this exact SHA out.
UPSTREAM_COMMIT = "077ea37d5ab4ff98bc567e4507f140dc4e5d5ad6"
UPSTREAM_REPO = "https://github.com/ufal/SimulStreaming"

# OpenAI Whisper multilingual "small" PyTorch checkpoint (NOT faster-whisper /
# CTranslate2, NOT small.en). Lives on Google Drive; the notebook validates the
# SHA-256 before loading.
MODEL_NAME = "small"
MODEL_FILENAME = "whisper-small.pt"
MODEL_SHA256 = "9ecf779972d90ba49c06d968637d720dd632c55bbf19d441fb42bf17a411e794"
# Canonical OpenAI download URL for the multilingual small checkpoint. Used only
# by the notebook's explicit "download to Drive" cell, never silently.
MODEL_DOWNLOAD_URL = (
    "https://openaipublic.azureedge.net/main/whisper/models/"
    "9ecf779972d90ba49c06d968637d720dd632c55bbf19d441fb42bf17a411e794/small.pt"
)

TARGET_RATE = 16000

# AlignAtt frame accounting. The Whisper audio frontend is identical across model
# sizes: 16 kHz -> 80-mel with hop 160 (100 mel frames/s) -> two conv layers with
# total stride 2 -> 50 encoder frames/s. So ONE encoder frame == 0.02 s for
# ``small`` exactly as documented upstream for ``large-v3``. ``frame_threshold``
# is therefore a tail-margin measured in 0.02 s units for every size, including
# the ``small`` model used here.
SECONDS_PER_FRAME = 0.02


def frame_threshold_seconds(frame_threshold: int) -> float:
    """Tail margin implied by ``frame_threshold`` for the Whisper small encoder."""
    return round(int(frame_threshold) * SECONDS_PER_FRAME, 4)


# --- Session / backend configuration ---------------------------------------

# Backend-specific tuning carried over the shared WebSocket protocol in the
# ``backend_config`` field (see ``stt_stream_protocol.validate_backend_config``).
# Each entry: name -> (python type, predicate). Validation is server-side and
# lives here rather than in the transport, so alternative engines never leak
# their flags into the faster-whisper path.
_CONFIG_FIELDS = {
    "model": (str, lambda v: bool(v)),
    "language": (str, lambda v: bool(v)),
    "task": (str, lambda v: v in ("transcribe", "translate")),
    "min_chunk_sec": (float, lambda v: v > 0),
    "beams": (int, lambda v: v >= 1),
    "use_vac": (bool, lambda v: True),
    "frame_threshold": (int, lambda v: v >= 1),
    "audio_max_len": (float, lambda v: v > 0),
    "audio_min_len": (float, lambda v: v >= 0),
    "never_fire": (bool, lambda v: True),
    "cif_ckpt_path": (str, lambda v: True),
}


@dataclass
class SimulStreamingConfig:
    """Effective configuration for one SimulStreaming run.

    Defaults follow the upstream ``official_default`` operating point, adapted to
    Spanish ``small`` for this thesis. ``frame_threshold`` default 25 == 0.5 s
    tail margin (see :data:`SECONDS_PER_FRAME`)."""

    model: str = MODEL_NAME
    language: str = "es"
    task: str = "transcribe"
    min_chunk_sec: float = 1.0
    beams: int = 1
    use_vac: bool = True
    frame_threshold: int = 25
    audio_max_len: float = 30.0
    audio_min_len: float = 0.0
    never_fire: bool = False
    cif_ckpt_path: str = ""
    model_path: str = ""

    @classmethod
    def from_overrides(cls, overrides: dict | None, base: "SimulStreamingConfig | None" = None) -> "SimulStreamingConfig":
        base = base or cls()
        if not overrides:
            return replace(base)
        unknown = sorted(set(overrides) - set(_CONFIG_FIELDS))
        if unknown:
            raise ValueError(f"unsupported SimulStreaming config override(s): {', '.join(unknown)}")
        normalized: dict = {}
        for name, value in overrides.items():
            converter, predicate = _CONFIG_FIELDS[name]
            if converter is bool:
                if not isinstance(value, bool):
                    raise ValueError(f"invalid {name}: expected boolean")
                converted = value
            else:
                if isinstance(value, bool):
                    raise ValueError(f"invalid {name}")
                try:
                    converted = converter(value)
                except (TypeError, ValueError) as exc:
                    raise ValueError(f"invalid {name}") from exc
            if not predicate(converted):
                raise ValueError(f"invalid {name}")
            normalized[name] = converted
        return replace(base, **normalized)

    @property
    def decoder_type(self) -> str:
        return "beam" if self.beams > 1 else "greedy"

    def as_effective_config(self) -> dict:
        data = asdict(self)
        data["decoder_type"] = self.decoder_type
        data["frame_threshold_sec"] = frame_threshold_seconds(self.frame_threshold)
        data["seconds_per_frame"] = SECONDS_PER_FRAME
        return data

    def run_config(self, *, realtime: bool, transport: str) -> dict:
        """Report block, deliberately parallel to the faster-whisper
        ``build_run_config`` so analysis/report code sees ``run_engine`` and the
        familiar ``config_*`` keys."""
        return {
            "run_engine": RUN_ENGINE,
            "upstream_commit": UPSTREAM_COMMIT,
            "model_sha256": MODEL_SHA256,
            "config_model": self.model,
            "config_language": self.language,
            "config_task": self.task,
            "config_min_chunk_sec": self.min_chunk_sec,
            "config_beams": self.beams,
            "config_decoder_type": self.decoder_type,
            "config_use_vac": self.use_vac,
            "config_frame_threshold": self.frame_threshold,
            "config_frame_threshold_sec": frame_threshold_seconds(self.frame_threshold),
            "config_audio_max_len": self.audio_max_len,
            "config_audio_min_len": self.audio_min_len,
            "config_never_fire": self.never_fire,
            "config_cif_ckpt_path": self.cif_ckpt_path or None,
            "config_realtime": realtime,
            "config_transport": transport,
        }


# --- Checkpoint validation -------------------------------------------------


def sha256_file(path: str | Path, *, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while block := handle.read(chunk_size):
            digest.update(block)
    return digest.hexdigest()


def validate_checkpoint(path: str | Path, expected_sha256: str = MODEL_SHA256) -> str:
    """Fail loudly for the wrong checkpoint (faster-whisper dir, CTranslate2 model,
    ``small.en``, or a corrupt download). Returns the verified SHA-256."""
    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(f"Whisper checkpoint not found: {path}")
    if path.is_dir():
        raise ValueError(
            f"{path} is a directory; expected a single OpenAI Whisper .pt file, "
            "not a faster-whisper / CTranslate2 model directory"
        )
    if path.suffix != ".pt":
        raise ValueError(f"{path} is not a .pt checkpoint (got suffix {path.suffix!r})")
    actual = sha256_file(path)
    if actual != expected_sha256:
        raise ValueError(
            "checkpoint SHA-256 mismatch: refusing to load\n"
            f"  expected {expected_sha256}\n"
            f"  actual   {actual}\n"
            f"  path     {path}"
        )
    return actual


# --- Audio helpers (numpy is available locally; torch is not) ---------------


def pcm_s16le_to_float32(pcm_bytes: bytes):
    """Decode interleaved little-endian S16 PCM to mono float32 in [-1, 1]."""
    import numpy as np

    if len(pcm_bytes) < 2:
        return np.zeros(0, dtype="float32")
    samples = np.frombuffer(pcm_bytes[: len(pcm_bytes) & ~1], dtype="<i2")
    return (samples.astype("float32") / 32768.0)


def resample_to_16k(mono_float32, source_rate: int):
    """Linear resample mono float32 audio from ``source_rate`` to 16 kHz.

    Good enough for an experimental streaming path; the board typically streams
    48 kHz. The upstream reference driver also feeds 16 kHz float32."""
    import numpy as np

    source_rate = int(source_rate)
    if source_rate == TARGET_RATE or mono_float32.size == 0:
        return np.asarray(mono_float32, dtype="float32")
    duration = mono_float32.size / float(source_rate)
    target_count = int(round(duration * TARGET_RATE))
    if target_count <= 0:
        return np.zeros(0, dtype="float32")
    source_positions = np.arange(mono_float32.size, dtype="float64") / source_rate
    target_positions = np.arange(target_count, dtype="float64") / TARGET_RATE
    return np.interp(target_positions, source_positions, mono_float32).astype("float32")


# --- Result mapping (fully testable with a fake processor) ------------------


def _coerce_float(value):
    if isinstance(value, bool) or value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def normalize_iter_result(result) -> dict | None:
    """Normalize one ``process_iter``/``finish`` return into a common shape.

    Upstream ``VACOnlineASRProcessor.process_iter`` returns a dict (with
    ``is_final`` appended on a VAC endpoint or ``finish``), or an empty dict when
    only VAD ran. Older/base processors return a ``(beg, end, text)`` tuple. We
    accept both. Returns ``None`` for an empty/no-text decode."""
    if result is None:
        return None
    if isinstance(result, dict):
        if not result:
            return None
        text = str(result.get("text", "") or "").strip()
        start = _coerce_float(result.get("start", result.get("beg")))
        end = _coerce_float(result.get("end"))
        is_final = bool(result.get("is_final", False))
        words = result.get("words") if isinstance(result.get("words"), list) else None
        truncated = bool(result.get("truncated_last_word", result.get("truncated", False)))
    elif isinstance(result, (tuple, list)) and len(result) >= 3:
        start = _coerce_float(result[0])
        end = _coerce_float(result[1])
        text = str(result[2] or "").strip()
        is_final = False
        words = None
        truncated = False
    else:
        raise TypeError(f"unsupported process_iter result: {result!r}")
    if not text and not is_final:
        return None
    return {
        "text": text,
        "start": start,
        "end": end,
        "is_final": is_final,
        "words": words,
        "truncated_last_word": truncated,
    }


class TranscriptAdapter:
    """Turn incremental AlignAtt commits into firmware-visible transcript events.

    Responsibilities the firmware relies on:

    * ``seq`` starts at 0 per session and grows monotonically.
    * ``text`` carries the *accumulated visible state* for the current segment
      (not just the delta), because the firmware replaces the current line.
    * The confirmed delta is preserved as ``delta_text`` metadata for analysis.
    * ``is_final=True`` when VAC closes a segment or on flush; the visible
      accumulator resets so the next segment starts clean, and the last text is
      never lost during flush.
    """

    def __init__(self, config: SimulStreamingConfig):
        self.config = config
        self.seq = 0
        self._segment_text = ""
        self._segment_start = None
        # Analysis counters.
        self.empty_decodes = 0
        self.vac_endpoints = 0
        self.truncations = 0
        self.updates = 0
        self.finals = 0

    def _emit(self, visible, start, end, is_final, normalized, infer_sec) -> dict:
        event = {
            "type": "transcript",
            "seq": self.seq,
            "is_final": bool(is_final),
            "start_sec": round(start, 3) if isinstance(start, (int, float)) else None,
            "end_sec": round(end, 3) if isinstance(end, (int, float)) else None,
            "text": visible,
            "delta_text": normalized["text"],
            "run_engine": RUN_ENGINE,
            "alignatt_frame_threshold": self.config.frame_threshold,
            "alignatt_frame_threshold_sec": frame_threshold_seconds(self.config.frame_threshold),
            "truncated_last_word": bool(normalized["truncated_last_word"]),
            "emit_monotonic": round(time.monotonic(), 6),
        }
        if normalized["words"] is not None:
            event["words"] = normalized["words"]
        if infer_sec is not None:
            event["gpu_infer_sec"] = round(float(infer_sec), 4)
        self.seq += 1
        return event

    def ingest(self, raw_result, *, vac_status=None, infer_sec=None) -> list[dict]:
        """Map one raw processor result to zero or one transcript event."""
        normalized = normalize_iter_result(raw_result)
        if normalized is None:
            self.empty_decodes += 1
            return []

        delta = normalized["text"]
        if delta:
            self._segment_text = (self._segment_text + " " + delta).strip() if self._segment_text else delta
        if self._segment_start is None and normalized["start"] is not None:
            self._segment_start = normalized["start"]
        if normalized["truncated_last_word"]:
            self.truncations += 1

        is_final = normalized["is_final"]
        visible = self._segment_text
        start = self._segment_start
        end = normalized["end"]
        event = self._emit(visible, start, end, is_final, normalized, infer_sec)
        if vac_status is not None:
            event["vac_status"] = vac_status
        if is_final:
            self.finals += 1
            self.vac_endpoints += 1
            self._segment_text = ""
            self._segment_start = None
        else:
            self.updates += 1
        return [event]

    def force_final(self, *, infer_sec=None) -> list[dict]:
        """Emit the pending visible text as final without new decoder output.

        Used on session close if the processor's ``finish`` did not already flush
        an is_final event, so the last segment is never dropped."""
        if not self._segment_text:
            return []
        normalized = {
            "text": "",
            "start": self._segment_start,
            "end": None,
            "is_final": True,
            "words": None,
            "truncated_last_word": False,
        }
        event = self._emit(self._segment_text, self._segment_start, None, True, normalized, infer_sec)
        event["forced_flush"] = True
        self.finals += 1
        self._segment_text = ""
        self._segment_start = None
        return event and [event] or []

    def stats_snapshot(self) -> dict:
        return {
            "empty_decodes": self.empty_decodes,
            "vac_endpoints": self.vac_endpoints,
            "last_word_truncations": self.truncations,
            "partial_updates": self.updates,
            "finals_emitted": self.finals,
            "events_emitted": self.seq,
        }


class SimulStreamingSession:
    """Per-WebSocket-session online processing state.

    Owns one upstream online processor plus the :class:`TranscriptAdapter`. Heavy
    model state is shared across sessions (see :class:`SharedSimulModel`); only
    the incremental streaming state here is per session."""

    def __init__(self, online, config: SimulStreamingConfig, source_rate: int = TARGET_RATE):
        self.online = online
        self.config = config
        self.source_rate = int(source_rate)
        self.adapter = TranscriptAdapter(config)
        self._finished = False

    def set_source_rate(self, source_rate: int) -> None:
        self.source_rate = int(source_rate)

    def _vac_status(self):
        return getattr(self.online, "status", None)

    def push_pcm(self, pcm_bytes: bytes) -> list[dict]:
        mono = pcm_s16le_to_float32(pcm_bytes)
        audio = resample_to_16k(mono, self.source_rate)
        return self.push_float32(audio)

    def push_float32(self, audio) -> list[dict]:
        self.online.insert_audio_chunk(audio)
        events: list[dict] = []
        # Drain everything ready this step. Upstream returns {} once idle.
        for _ in range(64):
            started = time.monotonic()
            result = self.online.process_iter()
            infer_sec = time.monotonic() - started
            normalized = normalize_iter_result(result)
            if normalized is None:
                self.adapter.empty_decodes += 1
                break
            events.extend(
                self.adapter.ingest(result, vac_status=self._vac_status(), infer_sec=infer_sec)
            )
            if not normalized["is_final"]:
                break
        return events

    def flush(self) -> list[dict]:
        if self._finished:
            return []
        self._finished = True
        events: list[dict] = []
        started = time.monotonic()
        result = self.online.finish()
        infer_sec = time.monotonic() - started
        events.extend(self.adapter.ingest(result, vac_status=self._vac_status(), infer_sec=infer_sec))
        events.extend(self.adapter.force_final())
        return events

    def stats_snapshot(self) -> dict:
        return self.adapter.stats_snapshot()


# --- Model loading (Colab / GPU only; imported lazily) ----------------------


class SharedSimulModel:
    """One loaded SimulStreaming ASR backend, reused across sessions.

    Importing SimulStreaming/torch happens here, not at module import, so the
    unit tests never touch a GPU. ``import_simulstreaming`` must have added the
    upstream checkout to ``sys.path`` first (the notebook does this)."""

    def __init__(self, config: SimulStreamingConfig):
        if not config.model_path:
            raise ValueError("SimulStreamingConfig.model_path is required to load a model")
        validate_checkpoint(config.model_path)
        self.config = config
        # Build the heavy ASR (loads the checkpoint) ONCE. Per-session online
        # processors are created cheaply around it in ``build_online``.
        self.asr, self._online_cls = self._build_asr(config)

    @staticmethod
    def _build_asr(config: SimulStreamingConfig):
        # Imported here so the module stays torch-free at import time.
        from simulstreaming_whisper import simul_asr_factory
        from types import SimpleNamespace

        # simul_asr_factory reads exactly these attributes (verified against upstream
        # commit 077ea37d5ab4ff98bc567e4507f140dc4e5d5ad6). ``log_level`` and the
        # prompt/context fields must be present or it raises AttributeError.
        args = SimpleNamespace(
            model_path=config.model_path,
            lan=config.language,
            task=config.task,
            beams=config.beams,
            decoder=config.decoder_type,
            audio_max_len=config.audio_max_len,
            audio_min_len=config.audio_min_len,
            frame_threshold=config.frame_threshold,
            cif_ckpt_path=(config.cif_ckpt_path or None),
            never_fire=config.never_fire,
            init_prompt=None,
            static_init_prompt=None,
            max_context_tokens=None,
            min_chunk_size=config.min_chunk_sec,
            logdir=None,
            log_level="INFO",
        )
        asr, template_online = simul_asr_factory(args)
        # Reuse the concrete online class the factory produced so we do not have to
        # guess its import path when spawning per-session processors.
        return asr, type(template_online)

    def build_online(self):
        """Create a fresh per-session online processor around the shared ASR.

        The factory does NOT wrap VAC; we do it here (matching upstream
        whisper_online_main.asr_factory: ``VACOnlineASRProcessor(min_chunk_size,
        online)``) so the heavy model stays shared while incremental + VAC state is
        per session."""
        online = self._online_cls(self.asr)
        if getattr(online, "init", None):
            try:
                online.init()
            except TypeError:
                pass
        if self.config.use_vac:
            from simulstreaming.whisper.whisper_streaming.vac_online_processor import (
                VACOnlineASRProcessor,
            )

            online = VACOnlineASRProcessor(self.config.min_chunk_sec, online)
            if getattr(online, "init", None):
                try:
                    online.init()
                except TypeError:
                    pass
        return online

    def warmup(self, seconds: float = 1.0):
        """Run a real warm-up decode so /health only flips to ready once inference
        actually works (never merely because uvicorn started)."""
        import numpy as np

        online = self.build_online()
        session = SimulStreamingSession(online, self.config, source_rate=TARGET_RATE)
        session.push_float32(np.zeros(int(TARGET_RATE * seconds), dtype="float32"))
        session.flush()
        return True


def transcribe_offline_float32(shared_model: SharedSimulModel, audio, source_rate: int) -> list[dict]:
    """Stream a whole clip through a fresh session and collect committed finals.

    This is the SimulStreaming pseudo-reference: same checkpoint, same language,
    same task as the live path, but fed the complete file. It is NOT the
    faster-whisper server."""
    session = SimulStreamingSession(shared_model.build_online(), shared_model.config, source_rate)
    finals: list[dict] = []
    partial_last = None
    import numpy as np

    audio = np.asarray(audio, dtype="float32")
    step = int(TARGET_RATE * max(shared_model.config.min_chunk_sec, 0.5))
    resampled = resample_to_16k(audio, source_rate)
    for start in range(0, resampled.size, step):
        for event in session.push_float32(resampled[start : start + step]):
            if event["is_final"]:
                finals.append(event)
            else:
                partial_last = event
    for event in session.flush():
        if event["is_final"]:
            finals.append(event)
        else:
            partial_last = event
    if not finals and partial_last is not None:
        partial_last = dict(partial_last, is_final=True)
        finals.append(partial_last)
    return finals
