import sys
import unittest
from collections import deque
from contextlib import contextmanager
from pathlib import Path

import numpy as np


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from server.evaluation import probe
from server.runtime import captions
from server.runtime import nemotron as backend
from server.runtime.nemotron import (
    LATENCY_TO_ATT_CONTEXT,
    MODEL_ID,
    NEMO_COMMIT,
    RUN_ENGINE,
    TARGET_RATE,
    NemotronConfig,
    NemotronSession,
    SharedNemotronModel,
    TranscriptAdapter,
    att_context_size_for,
    clean_text,
    common_word_prefix,
    install_prompt_projection_compat,
    map_committed_word_boundary,
    normalize_step_output,
    take_line_words,
)


class FakeStepOutput:
    """Duck-typed stand-in for NeMo's TranscribeStepOutput."""

    def __init__(self, partial_transcript="", final_transcript="", final_segments=None):
        self.stream_id = 0
        self.partial_transcript = partial_transcript
        self.final_transcript = final_transcript
        self.final_segments = final_segments or []


class FakeSegment:
    def __init__(self, text, start, end):
        self.text = text
        self.start = start
        self.end = end


class FakeEngine:
    """Stand-in for NemotronPipelineStream (the NeMo cache-aware stream)."""

    def __init__(self, frame_samples=1600):
        self.frame_samples = frame_samples
        self.scripted = deque()
        self.frames = []
        self.opened = 0
        self.closed = 0

    def script(self, *step_outputs):
        self.scripted.append(list(step_outputs))

    def open(self):
        self.opened += 1

    def close(self):
        self.closed += 1

    def step(self, samples, *, is_first, is_last, valid_length):
        self.frames.append(
            {
                "size": int(np.asarray(samples).size),
                "is_first": is_first,
                "is_last": is_last,
                "valid_length": valid_length,
            }
        )
        return self.scripted.popleft() if self.scripted else []


class FakePipeline:
    """Stand-in for NeMo's CacheAwareRNNTPipeline (used for shared-model tests)."""

    def __init__(self, chunk_size_in_secs=0.1):
        self.chunk_size_in_secs = chunk_size_in_secs
        self.reinit_calls = []
        self.last_init_config = None

    def init_parameters(self, cfg):
        self.last_init_config = cfg
        self.reinit_calls.append(("init_parameters", list(cfg["streaming"]["att_context_size"])))

    def init_bufferer_for_cache_aware_streaming(self):
        self.reinit_calls.append(("init_bufferer_for_cache_aware_streaming", None))

    def init_context_manager(self):
        self.reinit_calls.append(("init_context_manager", None))

    def init_endpointer(self):
        self.reinit_calls.append(("init_endpointer", None))


@contextmanager
def plain_pipeline_config():
    """Swap the OmegaConf wrapper for the plain dict so pipeline re-init is
    testable in WSL, where omegaconf (a NeMo dependency) is not installed."""
    original = backend.build_pipeline_config
    backend.build_pipeline_config = backend.pipeline_config_dict
    try:
        yield
    finally:
        backend.build_pipeline_config = original


class ProvenanceTests(unittest.TestCase):
    def test_pinned_identity_matches_the_validated_probe(self):
        self.assertEqual("nemotron_3_5_nemo", RUN_ENGINE)
        self.assertEqual("nvidia/nemotron-3.5-asr-streaming-0.6b", MODEL_ID)
        self.assertEqual("2639d4bef8d1450782263a8f616242acfb6fecb9", NEMO_COMMIT)
        self.assertEqual(16000, TARGET_RATE)

    def test_backend_and_probe_agree_on_shared_constants(self):
        # One operating-point table for the whole experiment: a silent drift here
        # would make the live server and the probe incomparable.
        self.assertEqual(probe.RUN_ENGINE, RUN_ENGINE)
        self.assertEqual(probe.MODEL_ID, MODEL_ID)
        self.assertEqual(probe.TARGET_SAMPLE_RATE, TARGET_RATE)
        self.assertEqual(probe.LATENCY_TO_ATT_CONTEXT, LATENCY_TO_ATT_CONTEXT)

    def test_display_policy_uses_shared_caption_limits(self):
        adapter = TranscriptAdapter(NemotronConfig())
        self.assertEqual(captions.VISIBLE_TEXT_MAX_CHARS, adapter.display_max_chars)
        self.assertEqual(captions.DISPLAY_LINE_MAX_CHARS, adapter.line_max_chars)


class ConfigTests(unittest.TestCase):
    def test_default_320ms_maps_to_att_context_56_3(self):
        config = NemotronConfig()
        self.assertEqual(320, config.latency_ms)
        self.assertEqual((56, 3), config.att_context_size)
        self.assertEqual("es-ES", config.target_lang)
        self.assertEqual("rnnt", config.decoder_type)
        self.assertTrue(config.strip_lang_tags)

    def test_every_published_latency_point_maps(self):
        self.assertEqual((56, 0), att_context_size_for(80))
        self.assertEqual((56, 6), att_context_size_for(560))
        self.assertEqual((56, 13), att_context_size_for(1120))

    def test_from_overrides_applies_and_validates(self):
        config = NemotronConfig.from_overrides({"latency_ms": 560, "stop_history_eou_ms": 400})
        self.assertEqual(560, config.latency_ms)
        self.assertEqual((56, 6), config.att_context_size)
        self.assertEqual(400, config.stop_history_eou_ms)

    def test_from_overrides_rejects_unknown_and_invalid(self):
        with self.assertRaises(ValueError):
            NemotronConfig.from_overrides({"beam_size": 4})
        with self.assertRaises(ValueError):
            NemotronConfig.from_overrides({"latency_ms": 250})  # not a published point
        with self.assertRaises(ValueError):
            NemotronConfig.from_overrides({"latency_ms": "fast"})
        with self.assertRaises(ValueError):
            NemotronConfig.from_overrides({"strip_lang_tags": 1})  # int, not bool
        with self.assertRaises(ValueError):
            NemotronConfig.from_overrides({"target_lang": "spanish"})
        with self.assertRaises(ValueError):
            NemotronConfig.from_overrides({"stop_history_eou_ms": -1})

    def test_constructor_rejects_non_rnnt_and_bad_locale(self):
        with self.assertRaises(ValueError):
            NemotronConfig(decoder_type="ctc")
        with self.assertRaises(ValueError):
            NemotronConfig(target_lang="es")

    def test_run_config_reports_engine_and_provenance(self):
        config = NemotronConfig()
        run_config = config.run_config(realtime=True, transport="websocket")
        self.assertEqual(RUN_ENGINE, run_config["run_engine"])
        self.assertEqual(NEMO_COMMIT, run_config["nemo_commit"])
        self.assertEqual(MODEL_ID, run_config["config_model"])
        self.assertEqual("es-ES", run_config["config_target_lang"])
        self.assertEqual("rnnt", run_config["config_decoder_type"])
        self.assertEqual([56, 3], run_config["config_att_context_size"])
        self.assertEqual(320, run_config["config_lookahead_ms"])
        self.assertTrue(run_config["config_strip_lang_tags"])
        self.assertTrue(run_config["config_realtime"])

    def test_effective_config_labels_320ms_as_lookahead(self):
        effective = NemotronConfig().as_effective_config()
        self.assertEqual(320, effective["lookahead_ms"])
        self.assertEqual([56, 3], effective["att_context_size"])
        self.assertEqual(16000, effective["target_sample_rate_hz"])


class TextCleaningTests(unittest.TestCase):
    def test_language_tags_are_stripped_and_whitespace_normalized(self):
        self.assertEqual("hola mundo", clean_text(" <es-ES> hola   mundo \n"))
        self.assertEqual("hola mundo", clean_text("hola mundo <es-ES>"))

    def test_capitalization_and_punctuation_are_preserved(self):
        self.assertEqual("Hola, ¿qué tal?", clean_text("Hola, ¿qué tal? <es-ES>"))

    def test_tags_are_kept_when_stripping_is_disabled(self):
        self.assertEqual("<es-ES> hola", clean_text("<es-ES> hola", strip_lang_tags=False))

    def test_internal_unknown_tokens_never_reach_visible_text(self):
        self.assertEqual("hola mundo", clean_text("hola <unk> ⁇ mundo"))
        self.assertEqual("texto", clean_text("<blank> <pad> <s> texto </s>"))

    def test_common_word_prefix(self):
        self.assertEqual(2, common_word_prefix(["a", "b"], ["a", "b", "c"]))
        self.assertEqual(0, common_word_prefix(["a"], ["b"]))

    def test_committed_boundary_survives_a_revised_first_word(self):
        previous = "La noticia importante sigue todavía pendiente".split()
        revised = "Esta noticia importante sigue todavía pendiente hoy".split()
        self.assertEqual(6, map_committed_word_boundary(previous, revised, 6))

    def test_committed_boundary_tracks_insertions_and_deletions(self):
        self.assertEqual(4, map_committed_word_boundary("uno dos tres".split(), "uno y dos tres".split(), 3))
        self.assertEqual(2, map_committed_word_boundary("uno y dos tres".split(), "uno dos tres".split(), 3))

    def test_take_line_words_cuts_on_word_boundaries(self):
        self.assertEqual(0, take_line_words([], 10))
        self.assertEqual(2, take_line_words(["hola", "mundo", "feliz"], 10))
        self.assertEqual(3, take_line_words(["a", "b", "c"], 10))
        # A single word longer than the line still advances (never a stuck loop),
        # and it is never cut in half.
        self.assertEqual(1, take_line_words(["supercalifragilisticoexpialidoso"], 5))

    def test_rollup_never_splits_a_word_in_half(self):
        adapter = TranscriptAdapter(NemotronConfig())
        words = [f"palabra{index}" for index in range(30)]
        events = adapter.ingest(FakeStepOutput(partial_transcript=" ".join(words)), audio_end_sec=5.0)
        events.extend(adapter.force_final(audio_end_sec=5.0))
        # Every word appears exactly once across the finalized lines, in order,
        # and no word is ever cut in half by the line-width roll-up.
        finalized = " ".join(event["full_text"] for event in events if event["is_final"]).split()
        self.assertEqual(words, finalized)


class NormalizeStepOutputTests(unittest.TestCase):
    def test_accepts_dataclass_like_and_dict(self):
        from_object = normalize_step_output(FakeStepOutput(partial_transcript="hola"))
        self.assertEqual("hola", from_object["partial_text"])
        from_dict = normalize_step_output({"final_transcript": "hola.", "partial_transcript": ""})
        self.assertEqual("hola.", from_dict["final_text"])

    def test_none_is_empty(self):
        self.assertEqual({"final_text": "", "partial_text": "", "final_segments": []},
                         normalize_step_output(None))


class TranscriptAdapterTests(unittest.TestCase):
    def setUp(self):
        self.adapter = TranscriptAdapter(NemotronConfig())

    def test_seq_starts_at_zero_and_increments(self):
        first = self.adapter.ingest(FakeStepOutput(partial_transcript="hola"), audio_end_sec=0.5)
        second = self.adapter.ingest(FakeStepOutput(partial_transcript="hola mundo"), audio_end_sec=1.0)
        self.assertEqual(0, first[0]["seq"])
        self.assertEqual(1, second[0]["seq"])

    def test_append_only_partials_emit_growing_visible_text(self):
        self.adapter.ingest(FakeStepOutput(partial_transcript="hola"), audio_end_sec=0.5)
        events = self.adapter.ingest(FakeStepOutput(partial_transcript="hola mundo"), audio_end_sec=1.0)
        self.assertEqual("hola mundo", events[0]["text"])
        self.assertFalse(events[0]["is_final"])

    def test_identical_partial_is_suppressed(self):
        self.adapter.ingest(FakeStepOutput(partial_transcript="hola"), audio_end_sec=0.5)
        events = self.adapter.ingest(FakeStepOutput(partial_transcript="hola"), audio_end_sec=1.0)
        self.assertEqual([], events)
        self.assertEqual(1, self.adapter.duplicate_partials_suppressed)
        self.assertEqual(2, self.adapter.partials_received)
        self.assertEqual(1, self.adapter.partials_emitted)

    def test_language_tags_never_reach_the_firmware(self):
        events = self.adapter.ingest(FakeStepOutput(partial_transcript="<es-ES> hola"), audio_end_sec=0.5)
        self.assertEqual("hola", events[0]["text"])
        self.assertNotIn("<es-ES>", events[0]["full_text"])

    def test_eou_produces_exactly_one_final(self):
        self.adapter.ingest(FakeStepOutput(partial_transcript="hola mundo"), audio_end_sec=1.0)
        events = self.adapter.ingest(
            FakeStepOutput(final_transcript=" hola mundo feliz"), audio_end_sec=1.5
        )
        finals = [event for event in events if event["is_final"]]
        self.assertEqual(1, len(finals))
        self.assertEqual("hola mundo feliz", finals[0]["text"])
        self.assertTrue(finals[0]["eou"])
        self.assertEqual("model_eou", finals[0]["final_reason"])
        self.assertEqual(1, self.adapter.eou_count)

    def test_eou_does_not_repeat_already_displayed_lines(self):
        long_partial = " ".join(["palabra"] * 20)  # > one display line
        promoted = self.adapter.ingest(FakeStepOutput(partial_transcript=long_partial), audio_end_sec=2.0)
        finals_before = [event for event in promoted if event["is_final"]]
        self.assertTrue(finals_before, "a long partial must roll up at least one line")
        events = self.adapter.ingest(FakeStepOutput(final_transcript=long_partial), audio_end_sec=2.5)
        emitted = " ".join(event["full_text"] for event in finals_before + events if event["is_final"])
        self.assertEqual(long_partial, emitted)

    def test_revised_final_does_not_repeat_already_displayed_history(self):
        old_words = ["vieja"] + [f"palabra{index}" for index in range(18)] + ["cola"]
        first = self.adapter.ingest(
            FakeStepOutput(partial_transcript=" ".join(old_words)), audio_end_sec=2.0
        )
        committed = " ".join(event["full_text"] for event in first if event["is_final"]).split()
        self.assertTrue(committed)

        revised_words = ["nueva"] + old_words[1:] + ["final"]
        second = self.adapter.ingest(
            FakeStepOutput(final_transcript=" ".join(revised_words)), audio_end_sec=2.5
        )
        newly_finalized = " ".join(
            event["full_text"] for event in second if event["is_final"]
        ).split()
        self.assertNotIn("nueva", newly_finalized)
        self.assertEqual(revised_words[len(committed):], newly_finalized)
        self.assertEqual(1, self.adapter.final_prefix_mismatches)

    def test_after_eou_the_next_utterance_starts_clean(self):
        self.adapter.ingest(FakeStepOutput(final_transcript="primera frase."), audio_end_sec=1.0)
        events = self.adapter.ingest(FakeStepOutput(partial_transcript="segunda"), audio_end_sec=1.5)
        self.assertEqual("segunda", events[0]["text"])

    def test_long_utterance_is_rolled_up_not_sent_whole(self):
        long_partial = " ".join(["palabra"] * 60)
        events = self.adapter.ingest(FakeStepOutput(partial_transcript=long_partial), audio_end_sec=30.0)
        self.assertGreater(len(events), 1)
        for event in events:
            self.assertLessEqual(len(event["text"]), captions.VISIBLE_TEXT_MAX_CHARS)

    def test_sentence_end_rolls_up_the_line(self):
        events = self.adapter.ingest(FakeStepOutput(partial_transcript="Hola mundo."), audio_end_sec=1.0)
        self.assertEqual(1, len(events))
        self.assertTrue(events[0]["is_final"])
        self.assertEqual("display_rollup", events[0]["final_reason"])
        self.assertNotIn("eou", events[0])

    def test_timestamps_are_always_numeric(self):
        events = self.adapter.ingest(FakeStepOutput(partial_transcript="hola"))
        self.assertIsInstance(events[0]["start_sec"], float)
        self.assertIsInstance(events[0]["end_sec"], float)

    def test_final_segment_timestamps_are_labelled_as_nemo(self):
        events = self.adapter.ingest(
            FakeStepOutput(final_transcript="hola", final_segments=[FakeSegment("hola", 0.25, 1.75)]),
            audio_end_sec=2.0,
        )
        self.assertEqual("nemo_segments", events[0]["timestamp_source"])
        self.assertEqual(1.75, events[0]["end_sec"])

    def test_sample_clock_timestamps_are_labelled_when_nemo_gives_none(self):
        events = self.adapter.ingest(FakeStepOutput(partial_transcript="hola"), audio_end_sec=0.5)
        self.assertEqual("sample_clock", events[0]["timestamp_source"])

    def test_partial_revision_is_counted_and_does_not_reshow_text(self):
        long_partial = " ".join(["palabra"] * 20)
        self.adapter.ingest(FakeStepOutput(partial_transcript=long_partial), audio_end_sec=2.0)
        promoted = self.adapter._promoted_words
        self.adapter.ingest(FakeStepOutput(partial_transcript="otra cosa"), audio_end_sec=2.5)
        self.assertEqual(1, self.adapter.partial_revisions)
        self.assertLessEqual(self.adapter._promoted_words, promoted)

    def test_force_final_flushes_pending_text_exactly_once(self):
        self.adapter.ingest(FakeStepOutput(partial_transcript="texto pendiente"), audio_end_sec=1.0)
        first = self.adapter.force_final(audio_end_sec=1.2)
        second = self.adapter.force_final(audio_end_sec=1.2)
        self.assertEqual(1, len(first))
        self.assertTrue(first[0]["is_final"])
        self.assertTrue(first[0]["forced_flush"])
        self.assertEqual([], second)
        self.assertEqual(1, self.adapter.flush_finals)

    def test_force_final_emits_nothing_when_everything_was_finalized(self):
        self.adapter.ingest(FakeStepOutput(final_transcript="todo listo."), audio_end_sec=1.0)
        self.assertEqual([], self.adapter.force_final(audio_end_sec=1.0))

    def test_last_frame_final_is_session_end_not_acoustic_eou(self):
        events = self.adapter.ingest(
            FakeStepOutput(final_transcript="cierre de sesión"),
            audio_end_sec=1.0,
            session_end=True,
        )
        self.assertEqual("session_end_final", events[-1]["final_reason"])
        self.assertTrue(events[-1]["session_end"])
        self.assertNotIn("eou", events[-1])
        self.assertEqual(0, self.adapter.eou_count)
        self.assertEqual(1, self.adapter.session_end_count)
        self.assertEqual(1, self.adapter.session_end_finals)

    def test_events_carry_engine_and_lookahead_provenance(self):
        events = self.adapter.ingest(FakeStepOutput(partial_transcript="hola"), audio_end_sec=0.5)
        event = events[0]
        self.assertEqual(RUN_ENGINE, event["run_engine"])
        self.assertEqual(NEMO_COMMIT, event["nemo_commit"])
        self.assertEqual("es-ES", event["target_lang"])
        self.assertEqual(320, event["lookahead_ms"])
        self.assertEqual([56, 3], event["att_context_size"])
        self.assertEqual("transcript", event["type"])

    def test_stats_snapshot_reports_required_counters(self):
        self.adapter.ingest(FakeStepOutput(partial_transcript="hola"), audio_end_sec=0.5)
        stats = self.adapter.stats_snapshot()
        for key in (
            "partials_received",
            "partials_emitted",
            "duplicate_partials_suppressed",
            "finals_emitted",
            "display_rollup_finals",
            "model_eou_finals",
            "session_end_finals",
            "eou_count",
            "session_end_count",
            "flush_finals",
            "events_emitted",
            "first_partial_audio_sec",
            "jobs_submitted",
            "events_dropped",
            "partial_jobs_skipped",
            "final_jobs_dropped",
            "event_queue_drained",
        ):
            self.assertIn(key, stats)

    def test_summary_does_not_store_hypothesis_history(self):
        for index in range(40):
            self.adapter.ingest(
                FakeStepOutput(partial_transcript=" ".join(["palabra"] * (index + 1))),
                audio_end_sec=float(index),
            )
        stats = self.adapter.stats_snapshot()
        for value in stats.values():
            self.assertNotIsInstance(value, (list, dict))


class SessionTests(unittest.TestCase):
    def test_pcm_s16le_is_decoded_and_resampled_to_16k(self):
        engine = FakeEngine(frame_samples=160)  # 10 ms frames
        session = NemotronSession(engine, NemotronConfig(), source_rate=48000)
        # 480 samples @48k == 160 samples @16k == exactly one frame.
        pcm = (np.zeros(480, dtype="<i2")).tobytes()
        session.push_pcm(pcm)
        self.assertEqual(1, len(engine.frames))
        self.assertEqual(160, engine.frames[0]["size"])
        self.assertTrue(engine.frames[0]["is_first"])
        self.assertFalse(engine.frames[0]["is_last"])
        self.assertEqual(0.01, session.stats_snapshot()["input_audio_sec"])

    def test_partial_frames_are_buffered_until_a_full_frame_exists(self):
        engine = FakeEngine(frame_samples=1600)
        session = NemotronSession(engine, NemotronConfig(), source_rate=TARGET_RATE)
        session.push_float32(np.zeros(800, dtype="float32"))
        self.assertEqual([], engine.frames)
        session.push_float32(np.zeros(800, dtype="float32"))
        self.assertEqual(1, len(engine.frames))

    def test_only_the_first_frame_is_marked_is_first(self):
        engine = FakeEngine(frame_samples=160)
        session = NemotronSession(engine, NemotronConfig(), source_rate=TARGET_RATE)
        session.push_float32(np.zeros(480, dtype="float32"))
        self.assertEqual([True, False, False], [frame["is_first"] for frame in engine.frames])

    def test_flush_pads_the_last_frame_and_marks_it_last(self):
        engine = FakeEngine(frame_samples=160)
        session = NemotronSession(engine, NemotronConfig(), source_rate=TARGET_RATE)
        session.push_float32(np.zeros(200, dtype="float32"))
        session.flush()
        last = engine.frames[-1]
        self.assertTrue(last["is_last"])
        self.assertEqual(160, last["size"])
        self.assertEqual(40, last["valid_length"])
        self.assertEqual(1, engine.closed)

    def test_flush_is_idempotent(self):
        engine = FakeEngine(frame_samples=160)
        session = NemotronSession(engine, NemotronConfig(), source_rate=TARGET_RATE)
        session.push_float32(np.zeros(160, dtype="float32"))
        session.flush()
        frames_after_first = len(engine.frames)
        self.assertEqual([], session.flush())
        self.assertEqual(frames_after_first, len(engine.frames))

    def test_final_from_last_engine_step_is_classified_as_session_end(self):
        engine = FakeEngine(frame_samples=160)
        engine.script()
        engine.script(FakeStepOutput(final_transcript="texto final"))
        session = NemotronSession(engine, NemotronConfig(), source_rate=TARGET_RATE)
        session.push_float32(np.zeros(160, dtype="float32"))
        events = session.flush()
        self.assertEqual("session_end_final", events[-1]["final_reason"])
        self.assertEqual(0, session.adapter.eou_count)
        self.assertEqual(1, session.adapter.session_end_count)

    def test_events_flow_from_engine_steps(self):
        engine = FakeEngine(frame_samples=160)
        engine.script(FakeStepOutput(partial_transcript="hola"))
        session = NemotronSession(engine, NemotronConfig(), source_rate=TARGET_RATE)
        events = session.push_float32(np.zeros(160, dtype="float32"))
        self.assertEqual(1, len(events))
        self.assertEqual("hola", events[0]["text"])
        self.assertEqual(0, events[0]["seq"])

    def test_each_session_has_independent_state(self):
        config = NemotronConfig()
        first = NemotronSession(FakeEngine(160), config, source_rate=TARGET_RATE)
        second = NemotronSession(FakeEngine(160), config, source_rate=TARGET_RATE)
        first.engine.script(FakeStepOutput(partial_transcript="hola"))
        first.push_float32(np.zeros(160, dtype="float32"))
        second.engine.script(FakeStepOutput(partial_transcript="otro"))
        events = second.push_float32(np.zeros(160, dtype="float32"))
        self.assertEqual(0, events[0]["seq"])  # seq restarts per session
        self.assertEqual(1, first.adapter.seq)

    def test_stats_snapshot_reports_streaming_and_rtf_metrics(self):
        engine = FakeEngine(frame_samples=160)
        session = NemotronSession(engine, NemotronConfig(), source_rate=TARGET_RATE)
        session.push_float32(np.zeros(1600, dtype="float32"))
        stats = session.stats_snapshot()
        self.assertEqual(10, stats["streaming_steps"])
        self.assertEqual(0.1, stats["processed_audio_sec"])
        self.assertEqual(320, stats["configured_lookahead_ms"])
        for key in (
            "input_audio_sec",
            "chunks_received",
            "inference_wall_sec",
            "inference_rtf",
            "max_backlog_samples",
            "session_errors",
        ):
            self.assertIn(key, stats)

    def test_engine_errors_are_counted_and_propagated(self):
        class BoomEngine(FakeEngine):
            def step(self, *args, **kwargs):
                raise RuntimeError("cuda blew up")

        session = NemotronSession(BoomEngine(160), NemotronConfig(), source_rate=TARGET_RATE)
        with self.assertRaises(RuntimeError):
            session.push_float32(np.zeros(160, dtype="float32"))
        self.assertEqual(1, session.session_errors)


class PipelineConfigTests(unittest.TestCase):
    def test_config_matches_the_official_cache_aware_rnnt_shape(self):
        cfg = backend.pipeline_config_dict(NemotronConfig())
        self.assertEqual("cache_aware", cfg["pipeline_type"])
        self.assertEqual("rnnt", cfg["asr_decoding_type"])
        self.assertEqual(MODEL_ID, cfg["asr"]["model_name"])
        self.assertEqual([56, 3], cfg["streaming"]["att_context_size"])
        self.assertEqual(16000, cfg["streaming"]["sample_rate"])
        self.assertEqual("frame", cfg["streaming"]["request_type"])
        self.assertEqual(800, cfg["endpointing"]["stop_history_eou"])
        self.assertEqual(2, cfg["endpointing"]["residue_tokens_at_end"])
        self.assertTrue(cfg["asr"]["decoding"]["strip_lang_tags"])
        self.assertEqual("greedy_batch", cfg["asr"]["decoding"]["strategy"])
        self.assertEqual("es-ES", cfg["lang"])
        # Deliberately off for this first integration.
        self.assertFalse(cfg["enable_itn"])
        self.assertFalse(cfg["enable_nmt"])
        self.assertFalse(cfg["asr"]["decoding"]["greedy"]["enable_per_stream_biasing"])

    def test_disabled_itn_keeps_constructor_required_config_shape(self):
        cfg = backend.pipeline_config_dict(NemotronConfig())
        self.assertFalse(cfg["enable_itn"])
        self.assertEqual(32, cfg["itn"]["batch_size"])
        self.assertEqual(16, cfg["itn"]["n_jobs"])
        self.assertEqual(4, cfg["itn"]["left_padding_size"])

    def test_latency_selects_the_attention_context(self):
        cfg = backend.pipeline_config_dict(NemotronConfig(latency_ms=560))
        self.assertEqual([56, 6], cfg["streaming"]["att_context_size"])

    def test_config_is_json_serializable_plain_data(self):
        import json

        json.dumps(backend.pipeline_config_dict(NemotronConfig()))


class SharedModelTests(unittest.TestCase):
    def test_prompt_compat_projects_encoder_output_before_decoding(self):
        class Decoding:
            def __init__(self):
                self.calls = []

            def rnnt_decoder_predictions_tensor(self, encoded, encoded_len, **kwargs):
                self.calls.append((encoded, encoded_len, kwargs))
                return ["hypothesis"]

        class Model:
            def __init__(self):
                self.decoding = Decoding()

        class Wrapper:
            def __init__(self):
                self.asr_model = Model()

            def execute_step(self, *args, **kwargs):
                raise AssertionError("the unpatched NeMo implementation must not run")

            def encoder_step(self, **kwargs):
                self.encoder_kwargs = kwargs
                return "raw-encoded", 7, "new-context"

        class Pipeline:
            prompt_enabled = True

            def __init__(self):
                self.asr_model = Wrapper()

        pipeline = Pipeline()
        projector_calls = []

        def projector(model, encoded, prompt_vectors):
            projector_calls.append((model, encoded, prompt_vectors))
            return "prompted-encoded"

        self.assertTrue(install_prompt_projection_compat(pipeline, projector=projector))
        result = pipeline.asr_model.execute_step(
            "signal", 123, "context", ["previous"], 2, False, prompt_vectors="es-ES-one-hot"
        )
        self.assertEqual((["hypothesis"], "new-context"), result)
        self.assertEqual("raw-encoded", projector_calls[0][1])
        self.assertEqual("es-ES-one-hot", projector_calls[0][2])
        decoding_call = pipeline.asr_model.asr_model.decoding.calls[0]
        self.assertEqual("prompted-encoded", decoding_call[0])
        self.assertEqual(["previous"], decoding_call[2]["partial_hypotheses"])
        # Installing twice must not wrap the wrapper twice.
        self.assertTrue(install_prompt_projection_compat(pipeline, projector=projector))

    def test_non_prompt_pipeline_does_not_install_compat(self):
        pipeline = FakePipeline()
        self.assertFalse(install_prompt_projection_compat(pipeline))

    def test_shared_pipeline_is_reused_across_sessions(self):
        pipeline = FakePipeline(chunk_size_in_secs=0.1)
        shared = SharedNemotronModel(NemotronConfig(), pipeline=pipeline)
        first = shared.build_session()
        second = shared.build_session()
        self.assertIs(pipeline, first.engine.pipeline)
        self.assertIs(pipeline, second.engine.pipeline)
        self.assertIsNot(first.adapter, second.adapter)
        self.assertEqual(1600, first.frame_samples)

    def test_set_latency_reruns_the_official_initialisers(self):
        pipeline = FakePipeline()
        shared = SharedNemotronModel(NemotronConfig(latency_ms=320), pipeline=pipeline)
        with plain_pipeline_config():
            shared.set_latency_ms(560)
        self.assertEqual(560, shared.config.latency_ms)
        self.assertEqual(
            [
                ("init_parameters", [56, 6]),
                ("init_bufferer_for_cache_aware_streaming", None),
                ("init_context_manager", None),
                ("init_endpointer", None),
            ],
            pipeline.reinit_calls,
        )

    def test_configure_streaming_applies_residue_and_stop_history_to_endpointer(self):
        pipeline = FakePipeline()
        shared = SharedNemotronModel(NemotronConfig(), pipeline=pipeline)
        requested = NemotronConfig(stop_history_eou_ms=500, residue_tokens_at_end=4)
        with plain_pipeline_config():
            shared.configure_streaming(requested)
        self.assertEqual(500, shared.config.stop_history_eou_ms)
        self.assertEqual(4, shared.config.residue_tokens_at_end)
        self.assertEqual(500, pipeline.last_init_config["endpointing"]["stop_history_eou"])
        self.assertEqual(4, pipeline.last_init_config["endpointing"]["residue_tokens_at_end"])
        self.assertIn(("init_endpointer", None), pipeline.reinit_calls)

    def test_set_latency_is_a_noop_for_the_same_point(self):
        pipeline = FakePipeline()
        shared = SharedNemotronModel(NemotronConfig(latency_ms=320), pipeline=pipeline)
        with plain_pipeline_config():
            shared.set_latency_ms(320)
        self.assertEqual([], pipeline.reinit_calls)

    def test_set_latency_rejects_unpublished_points(self):
        shared = SharedNemotronModel(NemotronConfig(), pipeline=FakePipeline())
        with plain_pipeline_config(), self.assertRaises(ValueError):
            shared.set_latency_ms(250)

    def test_provenance_reports_the_pinned_identity(self):
        shared = SharedNemotronModel(NemotronConfig(), pipeline=FakePipeline())
        provenance = shared.provenance()
        self.assertEqual(RUN_ENGINE, provenance["run_engine"])
        self.assertEqual(MODEL_ID, provenance["model_id"])
        self.assertEqual(NEMO_COMMIT, provenance["nemo_commit"])
        self.assertEqual([56, 3], provenance["att_context_size"])
        self.assertEqual("es-ES", provenance["target_lang"])
        self.assertIn("torch_version", provenance)
        self.assertIn("nemo_toolkit_version", provenance)
        self.assertIn("model_revision", provenance)

    def test_speech_canary_requires_at_least_one_streaming_event(self):
        class WarmupSession:
            def __init__(self, events):
                self.events = list(events)

            def push_float32(self, _audio):
                events, self.events = self.events, []
                return events

            def flush(self):
                return []

            def close(self):
                pass

            def stats_snapshot(self):
                return {
                    "events_emitted": 1,
                    "partials_received": 1,
                    "finals_emitted": 0,
                }

        shared = SharedNemotronModel(NemotronConfig(), pipeline=FakePipeline())
        sessions = iter((WarmupSession([]), WarmupSession([{"text": "hola"}])))
        shared.build_session = lambda **_kwargs: next(sessions)
        summary = shared.warmup(0.1, speech_audio=np.ones(1600, dtype="float32"))
        self.assertTrue(summary["speech_canary"])
        self.assertEqual(1, summary["events_emitted"])

    def test_speech_canary_rejects_an_all_blank_stream(self):
        class EmptySession:
            def push_float32(self, _audio):
                return []

            def flush(self):
                return []

            def close(self):
                pass

            def stats_snapshot(self):
                return {"events_emitted": 0}

        shared = SharedNemotronModel(NemotronConfig(), pipeline=FakePipeline())
        shared.build_session = lambda **_kwargs: EmptySession()
        with self.assertRaisesRegex(RuntimeError, "produced no transcript events"):
            shared.warmup(0.1, speech_audio=np.ones(1600, dtype="float32"))


class OfflineExtractionTests(unittest.TestCase):
    def test_text_is_cleaned_and_no_timestamps_are_invented(self):
        class Hypothesis:
            text = "<es-ES> hola mundo"
            timestamp = None

        text, segments = backend._extract_offline_text([Hypothesis()], strip_lang_tags=True)
        self.assertEqual("hola mundo", text)
        self.assertEqual([], segments)

    def test_real_nemo_timestamps_are_kept(self):
        class Hypothesis:
            text = "hola mundo"
            timestamp = {"segment": [{"start": 0.0, "end": 1.5, "segment": "hola mundo"}]}

        _text, segments = backend._extract_offline_text([Hypothesis()], strip_lang_tags=True)
        self.assertEqual([{"start_sec": 0.0, "end_sec": 1.5, "text": "hola mundo"}], segments)

    def test_empty_result_is_handled(self):
        self.assertEqual(("", []), backend._extract_offline_text([], strip_lang_tags=True))
        self.assertEqual(("", []), backend._extract_offline_text(None, strip_lang_tags=True))


class ImportContractTests(unittest.TestCase):
    def test_module_imports_without_torch_or_nemo(self):
        # The backend must import in WSL where neither torch nor NeMo exists.
        self.assertNotIn("torch", sys.modules)
        self.assertNotIn("nemo", sys.modules)


if __name__ == "__main__":
    unittest.main()
