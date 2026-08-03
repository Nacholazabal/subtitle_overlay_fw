#!/usr/bin/env python3
"""Nemotron 3.5 / NVIDIA NeMo cache-aware streaming STT backend adapter.

Third STT engine for the subtitle overlay thesis, next to faster-whisper and
SimulStreaming. It wraps the *official* NeMo streaming inference stack
(``nemo.collections.asr.inference``) pinned to commit
:data:`NEMO_COMMIT` so it can drive the *existing* pipeline unchanged:

    board -> bridge -> WebSocket -> firmware ACK -> HDMI overlay

This file deliberately mirrors ``stt_simulstreaming_backend`` in structure and
contracts; only the inference engine differs.

Design rules that make this module importable and testable everywhere:

* **No heavy imports at module load.** ``torch``/``numpy``/NeMo are imported
  lazily inside the functions that need them, so the module imports cleanly in
  the WSL test environment (no GPU, no NeMo). Only the model load / inference
  paths pull those dependencies, and those only run in Colab.
* **The streaming engine is injected.** :class:`NemotronSession` operates on any
  object satisfying the small :class:`StreamingEngine` contract
  (``frame_samples`` / ``open`` / ``step`` / ``close``). Unit tests pass a fake,
  so all result-mapping logic runs without NeMo present.

Official API used (verified by reading NeMo at :data:`NEMO_COMMIT`):

* ``nemo.collections.asr.inference.factory.pipeline_builder.PipelineBuilder``
  builds a ``CacheAwareRNNTPipeline`` (``pipeline_type=cache_aware``,
  ``asr_decoding_type=rnnt``).
* ``pipeline.open_session()`` / ``pipeline.transcribe_step([Frame])`` /
  ``pipeline.close_session()`` are the in-process streaming entry points; the
  encoder cache and previous hypotheses live in the pipeline's per-stream state,
  so nothing is recomputed and no subprocess is spawned per session.
* End-of-utterance is the official ``RNNTGreedyEndpointing`` driven by
  ``cfg.endpointing.stop_history_eou`` / ``residue_tokens_at_end``. It surfaces
  as a non-empty ``TranscribeStepOutput.final_transcript``; the in-progress text
  is ``TranscribeStepOutput.partial_transcript``.
* Prompt (language) conditioning for ``EncDecRNNTBPEModelWithPrompt`` is the
  official per-request ``ASRRequestOptions.language_code``; the pipeline resolves
  it through the checkpoint's ``model_defaults.prompt_dictionary``. It MUST be
  set explicitly: the pipeline's own default is ``en-US``.
"""

from __future__ import annotations

import re
import sys
import time
from dataclasses import asdict, dataclass, replace
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

# Display policy and the PCM/resample path are *shared* with the SimulStreaming
# backend on purpose: the firmware line limits and the roll-up caption behaviour
# must not diverge per engine. Both helpers are dependency-free at import time.
from scripts.stt_simulstreaming_backend import (
    DISPLAY_LINE_MAX_CHARS,
    SENTENCE_ENDINGS,
    VISIBLE_TEXT_MAX_CHARS,
    bounded_tail,
    pcm_s16le_to_float32,
    resample_to_16k,
)


# --- Provenance / pinned experiment identity -------------------------------

RUN_ENGINE = "nemotron_3_5_nemo"
MODEL_ID = "nvidia/nemotron-3.5-asr-streaming-0.6b"
# NeMo commit the probe validated on Colab (T4, torch 2.11.0+cu128, CUDA 12.8).
# Pinned: never install a floating ``main`` now that a working SHA is known.
NEMO_COMMIT = "2639d4bef8d1450782263a8f616242acfb6fecb9"
# Same remote the probe cloned, so the pinned SHA resolves identically.
NEMO_REPO = "https://github.com/NVIDIA-NeMo/NeMo.git"

TARGET_RATE = 16000
DECODER_TYPE = "rnnt"

# Official model-card mapping between the published latency operating points and
# the encoder attention context. Each right-context frame is 80 ms.
# Kept identical to ``stt_nemotron_probe.LATENCY_TO_ATT_CONTEXT`` (asserted by a test).
LATENCY_TO_ATT_CONTEXT = {
    80: (56, 0),
    160: (56, 1),
    320: (56, 3),
    560: (56, 6),
    1120: (56, 13),
}

# Same regex NeMo uses for ``strip_lang_tags`` (DEFAULT_LANG_TAG_PATTERN in
# nemo/collections/asr/parts/submodules/rnnt_decoding.py). The streaming
# inference pipeline detokenizes through ``ids_to_text_without_stripping`` /
# ``BPEDecoder``, which do NOT go through ``decode_tokens_to_str_with_strip_punctuation``,
# so a ``<es-ES>`` tag can still reach the pipeline text even with
# ``asr.decoding.strip_lang_tags=true``. We therefore apply the *same* official
# pattern once more on the way out instead of inventing a new one.
LANG_TAG_PATTERN = re.compile(r"\s*<[a-z]{2}-[A-Z]{2}>")

# Official defaults from examples/asr/conf/asr_streaming_inference/cache_aware_rnnt.yaml.
DEFAULT_STOP_HISTORY_EOU_MS = 800
DEFAULT_RESIDUE_TOKENS_AT_END = 2
DEFAULT_WORD_BOUNDARY_TOLERANCE = 4
DEFAULT_NUM_SLOTS = 8
DEFAULT_BATCH_SIZE = 1

_LOCALE_RE = re.compile(r"^[a-z]{2}-[A-Z]{2}$")


def att_context_size_for(latency_ms: int) -> tuple[int, int]:
    """Encoder attention context for a published latency operating point."""
    try:
        return LATENCY_TO_ATT_CONTEXT[int(latency_ms)]
    except (KeyError, TypeError, ValueError):
        supported = ", ".join(str(value) for value in sorted(LATENCY_TO_ATT_CONTEXT))
        raise ValueError(f"unsupported latency_ms={latency_ms!r}; choose {supported}") from None


# --- Session / backend configuration ---------------------------------------

# Backend-specific tuning carried over the shared WebSocket protocol in the
# ``backend_config`` field (see ``stt_stream_protocol.validate_backend_config``).
# Validation is server-side and lives here, exactly like the SimulStreaming
# backend, so alternative engines never leak flags into each other.
_CONFIG_FIELDS = {
    "target_lang": (str, lambda v: bool(_LOCALE_RE.match(v))),
    "latency_ms": (int, lambda v: v in LATENCY_TO_ATT_CONTEXT),
    "stop_history_eou_ms": (int, lambda v: v >= 0),
    "residue_tokens_at_end": (int, lambda v: v >= 0),
    "strip_lang_tags": (bool, lambda v: True),
    "asr_output_granularity": (str, lambda v: v in ("segment", "word")),
}


@dataclass
class NemotronConfig:
    """Effective configuration for one Nemotron run.

    Defaults are the operating point the Colab probe validated: Spanish
    ``es-ES``, RNNT, ``[56,3]`` (320 ms algorithmic lookahead), 16 kHz, language
    tags stripped. ``compute_dtype``/``use_amp`` follow the probe's T4 settings
    (float32 + AMP) rather than the Ampere-oriented ``bfloat16`` default in the
    upstream YAML."""

    model_id: str = MODEL_ID
    target_lang: str = "es-ES"
    latency_ms: int = 320
    decoder_type: str = DECODER_TYPE
    stop_history_eou_ms: int = DEFAULT_STOP_HISTORY_EOU_MS
    residue_tokens_at_end: int = DEFAULT_RESIDUE_TOKENS_AT_END
    strip_lang_tags: bool = True
    asr_output_granularity: str = "segment"
    compute_dtype: str = "float32"
    use_amp: bool = True
    device: str = "cuda"
    device_id: int = 0

    def __post_init__(self) -> None:
        if not str(self.model_id).strip():
            raise ValueError("model_id must not be empty")
        if not _LOCALE_RE.match(str(self.target_lang)):
            raise ValueError("target_lang must be a locale such as es-ES")
        if self.decoder_type != DECODER_TYPE:
            raise ValueError("Nemotron 3.5 backend currently requires decoder_type='rnnt'")
        # Raises for an unsupported operating point.
        att_context_size_for(self.latency_ms)
        if int(self.stop_history_eou_ms) < 0:
            raise ValueError("stop_history_eou_ms must be >= 0")
        if int(self.residue_tokens_at_end) < 0:
            raise ValueError("residue_tokens_at_end must be >= 0")
        if self.asr_output_granularity not in ("segment", "word"):
            raise ValueError("asr_output_granularity must be 'segment' or 'word'")

    @classmethod
    def from_overrides(cls, overrides: dict | None, base: "NemotronConfig | None" = None) -> "NemotronConfig":
        base = base or cls()
        if not overrides:
            return replace(base)
        unknown = sorted(set(overrides) - set(_CONFIG_FIELDS))
        if unknown:
            raise ValueError(f"unsupported Nemotron config override(s): {', '.join(unknown)}")
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
    def att_context_size(self) -> tuple[int, int]:
        return att_context_size_for(self.latency_ms)

    def as_effective_config(self) -> dict:
        data = asdict(self)
        data.update(
            {
                "run_engine": RUN_ENGINE,
                "att_context_size": list(self.att_context_size),
                "target_sample_rate_hz": TARGET_RATE,
                # 320 ms is the model's algorithmic right-context lookahead, NOT
                # the end-to-end board-to-HDMI latency.
                "lookahead_ms": self.latency_ms,
            }
        )
        return data

    def run_config(self, *, realtime: bool, transport: str) -> dict:
        """Report block, deliberately parallel to the faster-whisper
        ``build_run_config`` and the SimulStreaming ``run_config`` so analysis and
        report code sees ``run_engine`` and the familiar ``config_*`` keys."""
        left, right = self.att_context_size
        return {
            "run_engine": RUN_ENGINE,
            "nemo_commit": NEMO_COMMIT,
            "nemo_repo": NEMO_REPO,
            "config_model": self.model_id,
            "config_target_lang": self.target_lang,
            "config_decoder_type": self.decoder_type,
            "config_latency_ms": self.latency_ms,
            "config_lookahead_ms": self.latency_ms,
            "config_att_context_size": [left, right],
            "config_stop_history_eou_ms": self.stop_history_eou_ms,
            "config_residue_tokens_at_end": self.residue_tokens_at_end,
            "config_strip_lang_tags": self.strip_lang_tags,
            "config_asr_output_granularity": self.asr_output_granularity,
            "config_compute_dtype": self.compute_dtype,
            "config_use_amp": self.use_amp,
            "config_sample_rate_hz": TARGET_RATE,
            "config_realtime": realtime,
            "config_transport": transport,
        }


# --- Result mapping (fully testable with a fake engine) ---------------------


def clean_text(text, *, strip_lang_tags: bool = True) -> str:
    """Strip ``<es-ES>``-style language tags and normalize whitespace.

    Punctuation and capitalization produced by the model are preserved."""
    if not text:
        return ""
    text = str(text)
    if strip_lang_tags:
        text = LANG_TAG_PATTERN.sub("", text)
    return " ".join(text.split())


def _coerce_float(value):
    if isinstance(value, bool) or value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _segment_bounds(segments) -> tuple[float | None, float | None]:
    """Earliest start / latest end across NeMo ``TextSegment`` objects (or dicts)."""
    starts, ends = [], []
    for segment in segments or []:
        start = _coerce_float(segment.get("start") if isinstance(segment, dict) else getattr(segment, "start", None))
        end = _coerce_float(segment.get("end") if isinstance(segment, dict) else getattr(segment, "end", None))
        if start is not None:
            starts.append(start)
        if end is not None:
            ends.append(end)
    return (min(starts) if starts else None, max(ends) if ends else None)


def normalize_step_output(step_output) -> dict:
    """Normalize one NeMo ``TranscribeStepOutput`` into a plain dict.

    Accepts the real dataclass, a dict, or any duck-typed stand-in used by tests.
    ``final_transcript`` is non-empty exactly when the official endpointer fired
    (or on the last frame); ``partial_transcript`` is the in-progress hypothesis
    since the previous EOU."""
    if step_output is None:
        return {"final_text": "", "partial_text": "", "final_segments": []}
    if isinstance(step_output, dict):
        getter = step_output.get
    else:
        getter = lambda name, default=None: getattr(step_output, name, default)  # noqa: E731
    segments = getter("final_segments", None) or []
    return {
        "final_text": str(getter("final_transcript", "") or ""),
        "partial_text": str(getter("partial_transcript", "") or ""),
        "final_segments": list(segments),
    }


def take_line_words(words: list[str], max_chars: int) -> int:
    """How many leading ``words`` fit in ``max_chars`` (at least one).

    Word-exact counterpart of the SimulStreaming ``split_at_width``: this adapter
    promotes text to finished lines by *word count*, so the cut must land on a
    word boundary — a character cut would drop the tail of the cut word instead
    of carrying it to the next line."""
    if not words:
        return 0
    total = 0
    count = 0
    for word in words:
        extra = len(word) + (1 if count else 0)
        if count and total + extra > max_chars:
            break
        total += extra
        count += 1
    return max(count, 1)


def common_word_prefix(left: list[str], right: list[str]) -> int:
    """Number of leading words ``left`` and ``right`` share."""
    limit = min(len(left), len(right))
    index = 0
    while index < limit and left[index] == right[index]:
        index += 1
    return index


class TranscriptAdapter:
    """Turn NeMo streaming step outputs into firmware-visible transcript events.

    Responsibilities the firmware and bridge rely on (identical to the
    SimulStreaming adapter):

    * ``seq`` starts at 0 per session and grows monotonically.
    * ``start_sec``/``end_sec`` are always numeric (the bridge does
      ``float(end_sec)``).
    * ``text`` is the bounded, firmware-visible roll-up line; ``full_text``
      carries the same line for report reconstruction.
    * ``is_final=True`` only when a display line is completed — because the
      official endpointer fired (``eou=True``), because the line filled, because
      a sentence ended, or on the single session-close flush.

    Nemotron-specific adaptation: NeMo hands us the *whole* in-progress
    utterance every step (append-only in the probe), not a delta. We therefore
    track how many words of the current utterance have already been promoted to
    finalized lines and only ever display/emit the un-promoted remainder, so a
    60 s utterance never reaches the firmware as one 60 s line.
    """

    def __init__(self, config: NemotronConfig):
        self.config = config
        self.display_max_chars = VISIBLE_TEXT_MAX_CHARS
        self.line_max_chars = DISPLAY_LINE_MAX_CHARS
        self.seq = 0
        self._utterance_words: list[str] = []
        self._promoted_words = 0
        self._utterance_start_sec = None
        self._last_end_sec = 0.0
        self._last_partial_visible = None
        self._flushed = False
        # Analysis counters.
        self.partials_received = 0
        self.partials_emitted = 0
        self.duplicate_partials_suppressed = 0
        self.finals_emitted = 0
        self.display_rollup_finals = 0
        self.model_eou_finals = 0
        self.eou_count = 0
        self.flush_finals = 0
        self.partial_revisions = 0
        self.final_prefix_mismatches = 0
        self.empty_steps = 0
        self.first_partial_audio_sec = None

    # -- emission helpers ---------------------------------------------------

    def _emit(self, visible: str, *, is_final: bool, start_sec, end_sec, timestamp_source: str,
              infer_sec=None, eou: bool = False, forced_flush: bool = False,
              final_reason: str | None = None) -> dict:
        if not isinstance(start_sec, (int, float)) or isinstance(start_sec, bool):
            start_sec = self._last_end_sec
        if not isinstance(end_sec, (int, float)) or isinstance(end_sec, bool):
            end_sec = self._last_end_sec
        event = {
            "type": "transcript",
            "seq": self.seq,
            "is_final": bool(is_final),
            "start_sec": round(float(start_sec), 3),
            "end_sec": round(float(end_sec), 3),
            "text": bounded_tail(visible, self.display_max_chars),
            "full_text": visible,
            "run_engine": RUN_ENGINE,
            "nemo_commit": NEMO_COMMIT,
            "target_lang": self.config.target_lang,
            # Algorithmic right-context lookahead of the model, not end-to-end latency.
            "lookahead_ms": self.config.latency_ms,
            "att_context_size": list(self.config.att_context_size),
            "timestamp_source": timestamp_source,
            "emit_monotonic": round(time.monotonic(), 6),
        }
        if eou:
            event["eou"] = True
        if forced_flush:
            event["forced_flush"] = True
        if is_final:
            # ``is_final`` is the firmware/display contract: it promotes a line
            # in the roll-up overlay. It is deliberately broader than a model
            # end-of-utterance. Keep both meanings separate in logs and metrics.
            event["final_reason"] = final_reason or (
                "model_eou" if eou else "session_flush" if forced_flush else "display_rollup"
            )
        if infer_sec is not None:
            event["gpu_infer_sec"] = round(float(infer_sec), 4)
        self.seq += 1
        return event

    def _rollup(self, *, close_utterance: bool, start_sec, end_sec, timestamp_source: str,
                infer_sec=None, eou: bool = False, forced_flush: bool = False) -> list[dict]:
        """Promote filled/sentence-ending lines, then emit the remainder.

        When ``close_utterance`` the remainder is emitted as a final too (the
        utterance is over); otherwise it is emitted as a partial."""
        events: list[dict] = []
        line_start = start_sec
        while True:
            pending_words = self._utterance_words[self._promoted_words :]
            pending = " ".join(pending_words)
            if not pending:
                break
            ends_sentence = pending[-1] in SENTENCE_ENDINGS
            over_width = len(pending) > self.line_max_chars
            # A completed sentence rolls up immediately (broadcast roll-up
            # captions); on utterance close the tail is emitted by the block
            # below instead, so it is not promoted twice here.
            if not over_width and not (ends_sentence and not close_utterance):
                break
            promoted = len(pending_words) if not over_width else take_line_words(
                pending_words, self.line_max_chars
            )
            if promoted <= 0:
                break
            head = " ".join(pending_words[:promoted])
            events.append(
                self._emit(
                    head,
                    is_final=True,
                    start_sec=line_start,
                    end_sec=end_sec,
                    timestamp_source=timestamp_source,
                    infer_sec=infer_sec,
                    final_reason="display_rollup",
                )
            )
            self.finals_emitted += 1
            self.display_rollup_finals += 1
            self._promoted_words += promoted
            self._utterance_start_sec = end_sec
            line_start = end_sec
            self._last_partial_visible = None

        pending = " ".join(self._utterance_words[self._promoted_words :])
        if not pending:
            return events

        if close_utterance:
            events.append(
                self._emit(
                    pending,
                    is_final=True,
                    start_sec=line_start,
                    end_sec=end_sec,
                    timestamp_source=timestamp_source,
                    infer_sec=infer_sec,
                    eou=eou,
                    forced_flush=forced_flush,
                    final_reason="model_eou" if eou else "session_flush",
                )
            )
            self.finals_emitted += 1
            if eou:
                self.model_eou_finals += 1
            self._promoted_words = len(self._utterance_words)
            self._last_partial_visible = None
            return events

        if pending == self._last_partial_visible:
            self.duplicate_partials_suppressed += 1
            return events
        events.append(
            self._emit(
                pending,
                is_final=False,
                start_sec=line_start,
                end_sec=end_sec,
                timestamp_source=timestamp_source,
                infer_sec=infer_sec,
            )
        )
        self.partials_emitted += 1
        self._last_partial_visible = pending
        return events

    def _reset_utterance(self) -> None:
        self._utterance_words = []
        self._promoted_words = 0
        self._utterance_start_sec = None
        self._last_partial_visible = None

    # -- public API ---------------------------------------------------------

    def ingest(self, step_output, *, audio_start_sec=None, audio_end_sec=None, infer_sec=None) -> list[dict]:
        """Map one ``transcribe_step`` output to transcript events."""
        normalized = normalize_step_output(step_output)
        final_text = clean_text(normalized["final_text"], strip_lang_tags=self.config.strip_lang_tags)
        partial_text = clean_text(normalized["partial_text"], strip_lang_tags=self.config.strip_lang_tags)

        segment_start, segment_end = _segment_bounds(normalized["final_segments"])
        if audio_end_sec is not None:
            self._last_end_sec = float(audio_end_sec)

        events: list[dict] = []

        if final_text:
            # Official EOU fired: ``final_transcript`` is the authoritative text
            # for the utterance that just closed.
            words = final_text.split()
            shared = common_word_prefix(self._utterance_words[: self._promoted_words], words)
            if shared < self._promoted_words:
                # Text processing rewrote already-displayed words. Never re-show
                # them; resume from the longest common prefix instead.
                self.final_prefix_mismatches += 1
            self._utterance_words = words
            self._promoted_words = min(self._promoted_words, shared, len(words))
            if self._utterance_start_sec is None:
                self._utterance_start_sec = segment_start if segment_start is not None else audio_start_sec
            timestamp_source = "nemo_segments" if segment_end is not None else "sample_clock"
            events.extend(
                self._rollup(
                    close_utterance=True,
                    start_sec=self._utterance_start_sec,
                    end_sec=segment_end if segment_end is not None else self._last_end_sec,
                    timestamp_source=timestamp_source,
                    infer_sec=infer_sec,
                    eou=True,
                )
            )
            self.eou_count += 1
            self._reset_utterance()

        if partial_text:
            self.partials_received += 1
            if self.first_partial_audio_sec is None:
                self.first_partial_audio_sec = round(float(self._last_end_sec), 3)
            words = partial_text.split()
            shared = common_word_prefix(self._utterance_words[: self._promoted_words], words)
            if shared < self._promoted_words:
                # NeMo rebuilds ``partial_transcript`` from the live token buffer
                # each step, so it can in principle be revised. The probe saw
                # append-only growth; if it ever is not, resync without re-showing.
                self.partial_revisions += 1
            self._utterance_words = words
            self._promoted_words = min(self._promoted_words, shared, len(words))
            if self._utterance_start_sec is None:
                self._utterance_start_sec = audio_start_sec if audio_start_sec is not None else self._last_end_sec
            events.extend(
                self._rollup(
                    close_utterance=False,
                    start_sec=self._utterance_start_sec,
                    end_sec=self._last_end_sec,
                    timestamp_source="sample_clock",
                    infer_sec=infer_sec,
                )
            )
        elif not final_text:
            self.empty_steps += 1

        return events

    def force_final(self, *, audio_end_sec=None, infer_sec=None) -> list[dict]:
        """Flush the pending line exactly once on session close."""
        if self._flushed:
            return []
        self._flushed = True
        if audio_end_sec is not None:
            self._last_end_sec = float(audio_end_sec)
        if self._promoted_words >= len(self._utterance_words):
            return []
        events = self._rollup(
            close_utterance=True,
            start_sec=self._utterance_start_sec,
            end_sec=self._last_end_sec,
            timestamp_source="sample_clock",
            infer_sec=infer_sec,
            forced_flush=True,
        )
        self.flush_finals += len(events)
        self._reset_utterance()
        return events

    def stats_snapshot(self) -> dict:
        return {
            "partials_received": self.partials_received,
            "partials_emitted": self.partials_emitted,
            "duplicate_partials_suppressed": self.duplicate_partials_suppressed,
            "partial_revisions": self.partial_revisions,
            "final_prefix_mismatches": self.final_prefix_mismatches,
            "finals_emitted": self.finals_emitted,
            "display_rollup_finals": self.display_rollup_finals,
            "model_eou_finals": self.model_eou_finals,
            "eou_count": self.eou_count,
            "flush_finals": self.flush_finals,
            "empty_steps": self.empty_steps,
            "events_emitted": self.seq,
            "first_partial_audio_sec": self.first_partial_audio_sec,
            # Bench pipeline-reliability compatibility fields, same as the
            # SimulStreaming adapter: every emitted event is delivered, nothing
            # is discardable, so jobs == events and no drops. Without these the
            # bench reads jobs_submitted=0 and scores inference 0/0.
            "jobs_submitted": self.seq,
            "events_dropped": 0,
            "partial_jobs_skipped": 0,
            "final_jobs_dropped": 0,
            "event_queue_drained": True,
        }


# --- Per-session streaming state -------------------------------------------


class NemotronSession:
    """Per-WebSocket-session streaming state.

    Owns the audio residue buffer, the sample clock and the
    :class:`TranscriptAdapter`. The heavy model/pipeline is shared across
    sessions (see :class:`SharedNemotronModel`); only this incremental state is
    per session.

    ``engine`` is any object exposing the small streaming contract:
      * ``frame_samples`` — samples per cache-aware step at 16 kHz
      * ``open()`` / ``close()``
      * ``step(samples, is_first, is_last, valid_length) -> list[step_output]``
    """

    def __init__(self, engine, config: NemotronConfig, source_rate: int = TARGET_RATE):
        self.engine = engine
        self.config = config
        self.source_rate = int(source_rate)
        self.adapter = TranscriptAdapter(config)
        self.frame_samples = int(getattr(engine, "frame_samples", 0)) or 1
        self._residue = None
        self._samples_consumed = 0
        self._first_frame = True
        self._finished = False
        self._opened = False
        # Metrics.
        self.chunks_received = 0
        self.streaming_steps = 0
        self.input_audio_samples = 0
        self.infer_wall_sec = 0.0
        self.max_backlog_samples = 0
        self.session_errors = 0
        self.started_monotonic = time.monotonic()

    # -- lifecycle ----------------------------------------------------------

    def open(self) -> None:
        if not self._opened:
            self.engine.open()
            self._opened = True

    def close(self) -> None:
        if self._opened:
            self.engine.close()
            self._opened = False

    def set_source_rate(self, source_rate: int) -> None:
        self.source_rate = int(source_rate)

    # -- audio ingestion ----------------------------------------------------

    @property
    def audio_end_sec(self) -> float:
        return round(self._samples_consumed / float(TARGET_RATE), 3)

    def push_pcm(self, pcm_bytes: bytes) -> list[dict]:
        """PCM S16LE (board format) -> float32 mono -> 16 kHz -> streaming steps."""
        self.chunks_received += 1
        mono = pcm_s16le_to_float32(pcm_bytes)
        audio = resample_to_16k(mono, self.source_rate)
        return self.push_float32(audio)

    def push_float32(self, audio) -> list[dict]:
        import numpy as np

        self.open()
        audio = np.asarray(audio, dtype="float32")
        self.input_audio_samples += int(audio.size)
        if self._residue is not None and self._residue.size:
            audio = np.concatenate([self._residue, audio])
        self._residue = None

        events: list[dict] = []
        offset = 0
        while audio.size - offset >= self.frame_samples:
            frame = audio[offset : offset + self.frame_samples]
            offset += self.frame_samples
            events.extend(self._run_step(frame, valid_length=self.frame_samples, is_last=False))
        remainder = audio[offset:]
        self._residue = remainder if remainder.size else None
        self.max_backlog_samples = max(self.max_backlog_samples, int(remainder.size))
        return events

    def flush(self) -> list[dict]:
        """Feed the padded remainder as the last frame, then emit the final flush."""
        import numpy as np

        if self._finished:
            return []
        self._finished = True
        self.open()

        remainder = self._residue if self._residue is not None else np.zeros(0, dtype="float32")
        self._residue = None
        valid_length = int(remainder.size)
        frame = np.zeros(self.frame_samples, dtype="float32")
        if valid_length:
            frame[:valid_length] = remainder[: self.frame_samples]
            valid_length = min(valid_length, self.frame_samples)

        events = self._run_step(frame, valid_length=valid_length, is_last=True)
        events.extend(self.adapter.force_final(audio_end_sec=self.audio_end_sec))
        self.close()
        return events

    def _run_step(self, frame, *, valid_length: int, is_last: bool) -> list[dict]:
        start_sec = self.audio_end_sec
        started = time.monotonic()
        try:
            step_outputs = self.engine.step(
                frame, is_first=self._first_frame, is_last=is_last, valid_length=valid_length
            )
        except Exception:
            self.session_errors += 1
            raise
        infer_sec = time.monotonic() - started
        self.infer_wall_sec += infer_sec
        self.streaming_steps += 1
        self._first_frame = False
        self._samples_consumed += int(valid_length)
        end_sec = self.audio_end_sec

        events: list[dict] = []
        for step_output in step_outputs or []:
            events.extend(
                self.adapter.ingest(
                    step_output,
                    audio_start_sec=start_sec,
                    audio_end_sec=end_sec,
                    infer_sec=infer_sec,
                )
            )
        return events

    # -- metrics ------------------------------------------------------------

    def stats_snapshot(self) -> dict:
        # Samples are counted after resampling, in the model's 16 kHz clock.
        # Dividing by the board source rate (48 kHz) under-reported duration 3x.
        audio_sec = round(self.input_audio_samples / float(TARGET_RATE), 3)
        processed_sec = self.audio_end_sec
        stats = {
            "input_audio_sec": audio_sec,
            "processed_audio_sec": processed_sec,
            "chunks_received": self.chunks_received,
            "streaming_steps": self.streaming_steps,
            "frame_samples": self.frame_samples,
            "frame_ms": round(1000.0 * self.frame_samples / TARGET_RATE, 2),
            "inference_wall_sec": round(self.infer_wall_sec, 3),
            "inference_rtf": (
                round(self.infer_wall_sec / processed_sec, 4) if processed_sec > 0 else None
            ),
            "max_backlog_samples": self.max_backlog_samples,
            "max_backlog_ms": round(1000.0 * self.max_backlog_samples / TARGET_RATE, 2),
            "session_errors": self.session_errors,
            "configured_lookahead_ms": self.config.latency_ms,
            "session_wall_sec": round(time.monotonic() - self.started_monotonic, 3),
        }
        stats.update(self.adapter.stats_snapshot())
        return stats


# --- NeMo integration (Colab / GPU only; imported lazily) -------------------


def pipeline_config_dict(config: NemotronConfig) -> dict:
    """The official ``asr_streaming_inference`` cache-aware RNNT config, as a dict.

    Mirrors ``examples/asr/conf/asr_streaming_inference/cache_aware_rnnt.yaml``
    at :data:`NEMO_COMMIT`, with only the fields this experiment changes: the
    Nemotron 3.5 checkpoint, ``att_context_size`` for the chosen latency, and
    Spanish. ITN / NMT / biasing / beam search stay off.

    Kept dependency-free (plain dict) so the config shape is unit-testable in the
    WSL environment; :func:`build_pipeline_config` wraps it for NeMo."""
    left, right = config.att_context_size
    return {
        "asr": {
            "model_name": config.model_id,
            "device": config.device,
            "device_id": int(config.device_id),
            "compute_dtype": config.compute_dtype,
            "use_amp": bool(config.use_amp),
            "decoding": {
                "strategy": "greedy_batch",
                "preserve_alignments": False,
                "fused_batch_size": -1,
                "strip_lang_tags": bool(config.strip_lang_tags),
                "greedy": {
                    "use_cuda_graph_decoder": False,
                    "enable_per_stream_biasing": False,
                    "preserve_frame_confidence": False,
                    "max_symbols": 10,
                },
            },
        },
        # StreamingTextProcessor reads batch_size, n_jobs and
        # left_padding_size during construction even when enable_itn is false.
        # Keep the complete shape from NeMo's pinned
        # cache_aware_rnnt.yaml; these values configure no active ITN model in
        # this experiment because enable_itn remains false below.
        "itn": {
            "input_case": "lower_cased",
            "whitelist": None,
            "overwrite_cache": False,
            "max_number_of_permutations_per_split": 729,
            "left_padding_size": 4,
            "batch_size": 32,
            "n_jobs": 16,
        },
        "confidence": {
            "exclude_blank": True,
            "aggregation": "mean",
            "method_cfg": {
                "name": "entropy",
                "entropy_type": "tsallis",
                "alpha": 0.5,
                "entropy_norm": "exp",
            },
        },
        "endpointing": {
            "stop_history_eou": int(config.stop_history_eou_ms),
            "residue_tokens_at_end": int(config.residue_tokens_at_end),
        },
        "streaming": {
            "sample_rate": TARGET_RATE,
            "batch_size": DEFAULT_BATCH_SIZE,
            "word_boundary_tolerance": DEFAULT_WORD_BOUNDARY_TOLERANCE,
            "att_context_size": [left, right],
            "use_cache": True,
            "use_feat_cache": True,
            "chunk_size_in_secs": None,
            "request_type": "frame",
            "num_slots": DEFAULT_NUM_SLOTS,
        },
        "matmul_precision": "high",
        "log_level": 30,
        "pipeline_type": "cache_aware",
        "asr_decoding_type": config.decoder_type,
        "enable_itn": False,
        "enable_nmt": False,
        "asr_output_granularity": config.asr_output_granularity,
        "cache_dir": None,
        "lang": config.target_lang,
        "return_tail_result": False,
    }


def build_pipeline_config(config: NemotronConfig):
    """:func:`pipeline_config_dict` as the ``DictConfig`` NeMo's builder expects."""
    from omegaconf import OmegaConf

    return OmegaConf.create(pipeline_config_dict(config))


class NemotronPipelineStream:
    """Real :class:`NemotronSession` engine: one NeMo stream over the shared pipeline.

    Builds official ``Frame`` requests and drives
    ``pipeline.transcribe_step``. Encoder caches and previous hypotheses live in
    the pipeline's per-stream state, which is created on the first frame and
    deleted by NeMo when the last frame is submitted."""

    def __init__(self, pipeline, config: NemotronConfig, stream_id: int = 0):
        self.pipeline = pipeline
        self.config = config
        self.stream_id = int(stream_id)
        self.frame_samples = int(round(pipeline.chunk_size_in_secs * TARGET_RATE))
        if self.frame_samples <= 0:
            raise ValueError(f"invalid NeMo chunk size: {pipeline.chunk_size_in_secs!r}")
        self._options = None

    def _build_options(self):
        from nemo.collections.asr.inference.streaming.framing.request_options import ASRRequestOptions

        # ``language_code`` is the official prompt selector for
        # EncDecRNNTBPEModelWithPrompt. It MUST be set: the pipeline's own
        # default is en-US, which would silently transcribe Spanish as English.
        return ASRRequestOptions(
            language_code=self.config.target_lang,
            stop_history_eou=int(self.config.stop_history_eou_ms),
            asr_output_granularity=self.config.asr_output_granularity,
            enable_itn=False,
            enable_nmt=False,
        )

    def open(self) -> None:
        self.pipeline.open_session()
        self._options = self._build_options()

    def close(self) -> None:
        self.pipeline.close_session()

    def step(self, samples, *, is_first: bool, is_last: bool, valid_length: int) -> list:
        import torch
        from nemo.collections.asr.inference.streaming.framing.request import Frame

        tensor = torch.as_tensor(samples, dtype=torch.float32)
        frame = Frame(
            samples=tensor,
            stream_id=self.stream_id,
            is_first=bool(is_first),
            is_last=bool(is_last),
            length=int(valid_length),
            # Upstream MonoStream only attaches options to the first frame.
            options=self._options if is_first else None,
        )
        return self.pipeline.transcribe_step([frame])


class SharedNemotronModel:
    """One loaded NeMo cache-aware pipeline, reused across sessions.

    NeMo/torch are imported here, not at module import, so the unit tests never
    touch a GPU. Only one GPU session runs at a time (see the server's
    ``SingleSessionManager``), so the pipeline's per-stream state pool always
    holds at most one stream."""

    def __init__(self, config: NemotronConfig, *, pipeline=None):
        self.config = config
        self.pipeline = pipeline if pipeline is not None else self._build_pipeline(config)
        self.loaded_monotonic = time.monotonic()

    @staticmethod
    def _build_pipeline(config: NemotronConfig):
        from nemo.collections.asr.inference.factory.pipeline_builder import PipelineBuilder

        return PipelineBuilder.build_pipeline(build_pipeline_config(config))

    @property
    def chunk_size_in_secs(self) -> float:
        return float(getattr(self.pipeline, "chunk_size_in_secs", 0.0))

    def build_stream(self, config: NemotronConfig | None = None) -> NemotronPipelineStream:
        return NemotronPipelineStream(self.pipeline, config or self.config)

    def build_session(self, config: NemotronConfig | None = None, source_rate: int = TARGET_RATE) -> NemotronSession:
        config = config or self.config
        return NemotronSession(self.build_stream(config), config, source_rate=source_rate)

    def configure_streaming(self, config: NemotronConfig) -> None:
        """Apply pipeline-global streaming/endpointer settings between sessions.

        Language and ``stop_history_eou`` also travel in per-request options,
        but ``residue_tokens_at_end`` belongs to the pipeline endpointer. Apply
        the complete endpointing configuration here so every value reported in
        ``session_ready`` is actually active. The server enforces a single
        active GPU session, making this reinitialization safe.

        The initializers and their order match the pipeline constructor so every
        derived cache and context size stays consistent."""
        effective = replace(
            self.config,
            latency_ms=int(config.latency_ms),
            stop_history_eou_ms=int(config.stop_history_eou_ms),
            residue_tokens_at_end=int(config.residue_tokens_at_end),
        )
        att_context_size_for(effective.latency_ms)
        current = (
            self.config.latency_ms,
            self.config.stop_history_eou_ms,
            self.config.residue_tokens_at_end,
        )
        requested = (
            effective.latency_ms,
            effective.stop_history_eou_ms,
            effective.residue_tokens_at_end,
        )
        if requested == current:
            return
        self.config = effective
        cfg = build_pipeline_config(self.config)
        self.pipeline.init_parameters(cfg)
        self.pipeline.init_bufferer_for_cache_aware_streaming()
        self.pipeline.init_context_manager()
        self.pipeline.init_endpointer()

    def set_latency_ms(self, latency_ms: int) -> None:
        """Backward-compatible convenience wrapper for one operating point."""
        self.configure_streaming(replace(self.config, latency_ms=int(latency_ms)))

    def provenance(self) -> dict:
        """Exact versions/identities of everything that produced a transcript."""
        info = {
            "run_engine": RUN_ENGINE,
            "model_id": self.config.model_id,
            "nemo_commit": NEMO_COMMIT,
            "nemo_repo": NEMO_REPO,
            "target_lang": self.config.target_lang,
            "decoder_type": self.config.decoder_type,
            "att_context_size": list(self.config.att_context_size),
            "lookahead_ms": self.config.latency_ms,
            "sample_rate_hz": TARGET_RATE,
            "chunk_size_in_secs": self.chunk_size_in_secs,
            "compute_dtype": self.config.compute_dtype,
            "use_amp": self.config.use_amp,
        }
        info.update(runtime_provenance())
        info["model_revision"] = resolve_model_revision(self.pipeline)
        return info

    def warmup(self, seconds: float = 1.0) -> bool:
        """Run a real streaming decode so /health only flips to ready once
        inference actually works, not merely because uvicorn started."""
        import numpy as np

        session = self.build_session(source_rate=TARGET_RATE)
        session.push_float32(np.zeros(int(TARGET_RATE * max(seconds, 0.1)), dtype="float32"))
        session.flush()
        return True


def runtime_provenance() -> dict:
    """torch / CUDA / GPU / NeMo-toolkit versions, best effort."""
    info: dict = {}
    try:
        import torch

        info["torch_version"] = torch.__version__
        info["cuda_version"] = torch.version.cuda
        info["cuda_available"] = bool(torch.cuda.is_available())
        info["gpu_name"] = torch.cuda.get_device_name(0) if torch.cuda.is_available() else None
    except Exception as exc:  # noqa: BLE001 - provenance must never break /health
        info["torch_version"] = f"unavailable: {type(exc).__name__}"
    try:
        import nemo

        info["nemo_toolkit_version"] = getattr(nemo, "__version__", None)
    except Exception as exc:  # noqa: BLE001
        info["nemo_toolkit_version"] = f"unavailable: {type(exc).__name__}"
    return info


def resolve_model_revision(pipeline) -> str | None:
    """Resolved Hugging Face snapshot for the loaded checkpoint, if discoverable."""
    for attribute in ("_pretrained_model_revision", "_model_revision", "model_revision"):
        value = getattr(pipeline, attribute, None)
        if isinstance(value, str) and value:
            return value
    asr_wrapper = getattr(pipeline, "asr_model", None)
    asr_model = getattr(asr_wrapper, "asr_model", None)
    for holder in (asr_wrapper, asr_model):
        for attribute in ("model_revision", "_model_revision", "hf_revision"):
            value = getattr(holder, attribute, None)
            if isinstance(value, str) and value:
                return value
    restore_path = getattr(asr_model, "_save_restore_connector", None)
    path = getattr(restore_path, "model_extracted_dir", None) or getattr(asr_model, "_cfg_path", None)
    if isinstance(path, str) and path:
        match = re.search(r"snapshots/([0-9a-f]{8,40})", path)
        if match:
            return match.group(1)
    return None


# --- Offline (complete file) path ------------------------------------------


def strip_language_tags(text: str) -> str:
    return clean_text(text, strip_lang_tags=True)


def transcribe_offline_float32(shared_model: SharedNemotronModel, audio, config: NemotronConfig) -> dict:
    """Complete-file transcription with the *same* loaded checkpoint.

    Keeps the workaround the probe validated (``use_lhotse=False`` so the
    prompt-aware model falls back to the documented dynamic ``target_lang``
    prompt) by reusing ``stt_nemotron_probe.build_offline_transcribe_config``:
    one implementation, one place to fix."""
    import numpy as np

    from scripts.stt_nemotron_probe import NemotronProbeConfig, build_offline_transcribe_config

    model = shared_model.pipeline.asr_model.asr_model
    probe_config = NemotronProbeConfig(
        model_id=config.model_id,
        target_lang=config.target_lang,
        latency_ms=config.latency_ms,
        decoder_type=config.decoder_type,
        strip_lang_tags=config.strip_lang_tags,
    )
    transcribe_config = build_offline_transcribe_config(model, probe_config)
    audio = np.asarray(audio, dtype="float32")
    hypotheses = model.transcribe(audio=[audio], override_config=transcribe_config)
    text, segments = _extract_offline_text(hypotheses, strip_lang_tags=config.strip_lang_tags)
    return {"text": text, "segments": segments}


def _extract_offline_text(hypotheses, *, strip_lang_tags: bool) -> tuple[str, list[dict]]:
    """Pull text (and timestamps only if NeMo actually produced them)."""
    if hypotheses is None:
        return "", []
    if isinstance(hypotheses, (list, tuple)):
        if not hypotheses:
            return "", []
        # RNNT transcribe() may return (best, all-beams); take the best list.
        if len(hypotheses) == 2 and isinstance(hypotheses[0], (list, tuple)) and hypotheses[0]:
            hypotheses = hypotheses[0]
        first = hypotheses[0]
    else:
        first = hypotheses
    text = getattr(first, "text", None)
    if text is None:
        text = first if isinstance(first, str) else ""
    text = clean_text(text, strip_lang_tags=strip_lang_tags)

    segments: list[dict] = []
    timestamp = getattr(first, "timestamp", None)
    raw_segments = timestamp.get("segment") if isinstance(timestamp, dict) else None
    for segment in raw_segments or []:
        if not isinstance(segment, dict):
            continue
        start = _coerce_float(segment.get("start"))
        end = _coerce_float(segment.get("end"))
        piece = clean_text(segment.get("segment", segment.get("text", "")), strip_lang_tags=strip_lang_tags)
        if piece and start is not None and end is not None:
            segments.append({"start_sec": round(start, 3), "end_sec": round(end, 3), "text": piece})
    return text, segments
