#!/usr/bin/env python3
"""Receive board PCM audio and transcribe it with faster-whisper."""

import argparse
import json
import queue
import socket
import struct
import threading
import time
import wave


STREAM_MAGIC = b"SAUDPCM\x00"
STREAM_HEADER = "!6I"
CHUNK_HEADER = "!QQ5I"
FORMAT_S16_LE = 1

WHISPER_RATE = 16000
EXPECTED_CHANNELS = 1
EXPECTED_SAMPLE_WIDTH = 2

# Phrases Whisper commonly hallucinates over silence/music; drop them so they do
# not flash on screen as real subtitles.
HALLUCINATION_MARKERS = (
    "amara.org",
    "subtítulos realizados por",
    "subtitulos realizados por",
    "suscríbete al canal",
    "gracias por ver el video",
    "gracias por ver este video",
)


WORD_COMPARE_STRIP = ".,;:!?¿¡\"'()[]{}"


def is_hallucination(text):
    lowered = text.lower()
    return any(marker in lowered for marker in HALLUCINATION_MARKERS)


def comparable_word(word):
    return word.strip(WORD_COMPARE_STRIP).casefold()


def word_prefix_len(left, right):
    left_words = left.split()
    right_words = right.split()
    limit = min(len(left_words), len(right_words))
    index = 0
    while (
        index < limit
        and comparable_word(left_words[index]) == comparable_word(right_words[index])
    ):
        index += 1
    return index


def comparable_text(text):
    return " ".join(comparable_word(word) for word in text.split())


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("connection closed")
        data.extend(chunk)
    return bytes(data)


def pcm_s16le_to_float32(payload):
    import numpy as np

    samples = np.frombuffer(payload, dtype="<i2")
    return samples.astype(np.float32) / 32768.0


def resample_audio(samples, source_rate, target_rate):
    if source_rate == target_rate:
        return samples

    import numpy as np

    if samples.size == 0:
        return samples

    target_size = int(round(samples.size * target_rate / source_rate))
    if target_size <= 0:
        return np.empty(0, dtype=np.float32)

    source_positions = np.arange(samples.size, dtype=np.float64)
    target_positions = np.arange(target_size, dtype=np.float64) * source_rate / target_rate
    resampled = np.interp(target_positions, source_positions, samples)
    return resampled.astype(np.float32)


class TranscriptSink:
    def handle_event(self, event):
        raise NotImplementedError

    def close(self):
        pass


class ConsoleTranscriptSink(TranscriptSink):
    def handle_event(self, event):
        final_mark = "final" if event["is_final"] else "partial"
        print(
            f"stt {final_mark}#{event['seq']} "
            f"t={event['start_sec']:.3f}..{event['end_sec']:.3f}s "
            f"text={event['text']!r}",
            flush=True,
        )


class JsonlTranscriptSink(TranscriptSink):
    def __init__(self, path):
        self.file = open(path, "w", encoding="utf-8")

    def handle_event(self, event):
        self.file.write(json.dumps(event, ensure_ascii=False) + "\n")
        self.file.flush()

    def close(self):
        self.file.close()


class TcpTranscriptSink(TranscriptSink):
    def __init__(self, host, port, max_queue=32):
        self.host = host
        self.port = port
        self.sock = None
        self.events = queue.Queue(maxsize=max_queue)
        self.stop_event = threading.Event()
        self.worker = threading.Thread(target=self._worker_main, daemon=True)
        self.worker.start()

    def handle_event(self, event):
        try:
            self.events.put_nowait(event)
            return
        except queue.Full:
            pass

        if not event.get("is_final", False):
            print("subtitle TCP queue full: dropping partial transcript", flush=True)
            return

        if self._drop_one_partial_from_queue():
            try:
                self.events.put_nowait(event)
                return
            except queue.Full:
                pass

        print("subtitle TCP queue full: dropping final transcript", flush=True)

    def _drop_one_partial_from_queue(self):
        kept = []
        dropped = False

        while True:
            try:
                queued = self.events.get_nowait()
            except queue.Empty:
                break

            if (not dropped) and (not queued.get("is_final", False)):
                dropped = True
                continue

            kept.append(queued)

        for queued in kept:
            try:
                self.events.put_nowait(queued)
            except queue.Full:
                break

        return dropped

    def _connect(self):
        if self.sock is None:
            self.sock = socket.create_connection((self.host, self.port), timeout=2.0)
            self.sock.settimeout(2.0)
            print(f"subtitle TCP connected to {self.host}:{self.port}", flush=True)

    def _send_event(self, event):
        line = json.dumps(self._wire_event(event), ensure_ascii=False) + "\n"
        self._connect()
        self.sock.sendall(line.encode("utf-8"))

    @staticmethod
    def _wire_event(event):
        """Keep the firmware-facing NDJSON small and contract-only."""
        return {
            "seq": event["seq"],
            "is_final": event["is_final"],
            "start_sec": event["start_sec"],
            "end_sec": event["end_sec"],
            "text": event["text"],
        }

    def _worker_main(self):
        while not self.stop_event.is_set():
            try:
                event = self.events.get(timeout=0.1)
            except queue.Empty:
                continue

            while not self.stop_event.is_set():
                try:
                    self._send_event(event)
                    break
                except OSError as exc:
                    print(f"subtitle TCP send failed: {exc}", flush=True)
                    self._close_socket()
                    self.stop_event.wait(0.5)

    def _close_socket(self):
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def close(self):
        self.stop_event.set()
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self._close_socket()
        self.worker.join(timeout=2.0)


class CompositeTranscriptSink(TranscriptSink):
    def __init__(self, sinks):
        self.sinks = sinks

    def handle_event(self, event):
        for sink in self.sinks:
            sink.handle_event(event)

    def close(self):
        for sink in self.sinks:
            sink.close()


class PartialStabilityFilter:
    """Suppress unstable partial hypotheses using word-prefix local agreement."""

    def __init__(self, agreement):
        if agreement < 1:
            raise ValueError("agreement must be positive")
        self.agreement = agreement
        self.history = []
        self.last_emitted_partial = ""
        self.last_emitted_partial_key = ""

    def handle_event(self, event):
        if event.get("is_final", False):
            self.reset()
            return [event]

        if self.agreement <= 1:
            return [event]

        text = event.get("text", "").strip()
        if not text:
            return []

        self.history.append(text)
        self.history = self.history[-self.agreement :]
        if len(self.history) < self.agreement:
            return []

        stable_words = self.history[0].split()
        stable_len = len(stable_words)
        for candidate in self.history[1:]:
            stable_len = min(stable_len, word_prefix_len(" ".join(stable_words), candidate))

        stable_text = " ".join(stable_words[:stable_len]).strip()
        stable_key = comparable_text(stable_text)
        if not stable_text or stable_key == self.last_emitted_partial_key:
            return []

        stabilized = dict(event)
        stabilized["text"] = stable_text
        self.last_emitted_partial = stable_text
        self.last_emitted_partial_key = stable_key
        return [stabilized]

    def reset(self):
        self.history = []
        self.last_emitted_partial = ""
        self.last_emitted_partial_key = ""


class FasterWhisperEngine:
    def __init__(
        self,
        model_size,
        device,
        compute_type,
        language,
        beam_size,
        vad_filter,
        cpu_threads=0,
    ):
        from faster_whisper import WhisperModel

        print(
            f"loading faster-whisper model={model_size} device={device} "
            f"compute_type={compute_type} cpu_threads={cpu_threads or 'auto'}",
            flush=True,
        )
        model_kwargs = {}
        if cpu_threads > 0:
            model_kwargs["cpu_threads"] = cpu_threads
        self.model = WhisperModel(model_size, device=device, compute_type=compute_type, **model_kwargs)
        self.language = language
        self.beam_size = beam_size
        self.vad_filter = vad_filter
        self.seq = 0

    def transcribe_chunk(self, audio, start_sec, end_sec, is_final):
        segments, _info = self.model.transcribe(
            audio,
            language=self.language,
            beam_size=self.beam_size,
            vad_filter=self.vad_filter,
        )
        text = " ".join(segment.text.strip() for segment in segments).strip()
        if not text:
            return None

        if is_hallucination(text):
            print(f"stt dropping hallucination: {text!r}", flush=True)
            return None

        # seq is monotonic across partials and finals so the firmware (which drops
        # seq <= last_seq) always advances to the newest hypothesis.
        event = {
            "seq": self.seq,
            "is_final": bool(is_final),
            "start_sec": round(start_sec, 3),
            "end_sec": round(end_sec, 3),
            "text": text,
        }
        self.seq += 1
        return event


class ColabWhisperEngine:
    """Remote inference via Colab notebook server."""

    def __init__(self, colab_url, language, beam_size, vad_filter, timeout=30.0):
        import requests

        self.colab_url = colab_url.rstrip("/")
        self.session = requests.Session()
        self.timeout = timeout
        self.seq = 0
        print(f"connecting to Colab inference server at {self.colab_url}", flush=True)

        # Health check
        try:
            resp = self.session.get(f"{self.colab_url}/health", timeout=5.0)
            resp.raise_for_status()
            info = resp.json()
            print(
                f"colab server ready: model={info.get('model')} device={info.get('device')}",
                flush=True,
            )
        except Exception as exc:
            raise RuntimeError(f"colab server health check failed: {exc}") from exc

    def transcribe_chunk(self, audio, start_sec, end_sec, is_final):
        import base64

        import numpy as np

        # Send audio as base64-encoded float32 to avoid precision loss
        audio_float32 = audio.astype(np.float32)
        audio_bytes = audio_float32.tobytes()
        audio_b64 = base64.b64encode(audio_bytes).decode("ascii")

        payload = {
            "audio_base64": audio_b64,
            "start_sec": start_sec,
            "end_sec": end_sec,
            "is_final": is_final,
            "seq": self.seq,
        }

        try:
            resp = self.session.post(
                f"{self.colab_url}/transcribe", json=payload, timeout=self.timeout
            )
            resp.raise_for_status()
            result = resp.json()

            inference_sec = result.get("inference_sec", 0.0)
            event = result.get("event")
            if event is None:
                return None
            try:
                event["remote_infer_sec"] = round(float(inference_sec), 3)
            except (TypeError, ValueError):
                pass

            # Colab already set the seq, but we track it locally too
            self.seq = event["seq"] + 1
            return event
        except Exception as exc:
            print(f"colab transcription request failed: {exc}", flush=True)
            return None


class ChunkTranscriber:
    def __init__(
        self,
        engine,
        sink,
        target_rate,
        max_window_sec,
        partial_sec=0.7,
        min_silence_sec=0.5,
        gain=0.0,
        partial_agreement=1,
        max_pending_chunks=4,
        drop_oldest=True,
        realtime=True,
        run_config=None,
    ):
        import numpy as np
        from faster_whisper.vad import VadOptions

        self.engine = engine
        self.sink = sink
        self.partial_filter = PartialStabilityFilter(partial_agreement)
        # gain > 0 applies a fixed multiplier; gain == 0 auto-normalizes each
        # phrase to a healthy peak (fixes a too-quiet capture without a magic
        # constant, and keeps working if the hardware level is later raised).
        self.gain = gain
        self._auto_peak = 0.0
        # Live audio must drop backlog to stay real-time; offline files instead
        # apply backpressure so no chunk is lost.
        self.drop_oldest = drop_oldest
        self.realtime = realtime
        self.source_rate = target_rate
        self.target_rate = target_rate
        self.max_window_samples = int(round(target_rate * max_window_sec))
        self.min_silence_samples = int(round(target_rate * min_silence_sec))
        self.partial_samples = int(round(target_rate * partial_sec)) if partial_sec > 0.0 else 0
        self.max_window_sec = float(max_window_sec)
        self.min_silence_sec = float(min_silence_sec)
        self.partial_sec = float(partial_sec)
        self.partial_agreement = int(partial_agreement)
        self.run_config = dict(run_config or {})
        self._audio_start_monotonic = None
        self._job_seq = 0
        self._dropped_jobs = 0
        self._last_vad_metrics = {}
        self._queue_lock = threading.Lock()
        self._partial_jobs_outstanding = 0
        # Tight VAD segments so the trailing-silence measurement is accurate; the
        # finalize decision uses our own min_silence threshold below.
        self._vad_options = VadOptions(min_silence_duration_ms=100, speech_pad_ms=30)
        # window holds the current (growing) utterance until it is finalized.
        self.window = np.empty(0, dtype=np.float32)
        self.window_start_sample = 0
        self.samples_since_partial = 0
        self.pending_chunks = queue.Queue(maxsize=max_pending_chunks)
        self.stop_event = threading.Event()
        self.worker = threading.Thread(target=self._worker_main, daemon=True)
        self.worker.start()

    def set_source_rate(self, source_rate):
        self.source_rate = source_rate
        if source_rate != self.target_rate:
            print(
                f"resampling audio from {source_rate} Hz to {self.target_rate} Hz for Whisper",
                flush=True,
            )

    def push_pcm(self, payload):
        import numpy as np

        samples = pcm_s16le_to_float32(payload)
        samples = resample_audio(samples, self.source_rate, self.target_rate)
        if samples.size == 0:
            return

        # Apply gain at the input so VAD segmentation and transcription both see a
        # healthy level (a too-quiet capture otherwise breaks VAD boundaries).
        samples = self._apply_gain(np, samples)
        if self._audio_start_monotonic is None:
            self._audio_start_monotonic = time.monotonic()

        self.window = np.concatenate((self.window, samples))
        self.samples_since_partial += samples.size

        # Cut the phrase at a real pause (or a forced cap), then emit a low-latency
        # partial for the audio accumulated since the last emission.
        finalized = self._segment()
        if (
            not finalized
            and self.partial_samples > 0
            and self.window.size > 0
            and self.samples_since_partial >= self.partial_samples
        ):
            self._emit_partial()

    def flush(self):
        if self.window.size > 0:
            self._finalize_upto(self.window.size,
                                reason="flush",
                                vad_metrics=self._last_vad_metrics)
        self.pending_chunks.join()

    def close(self):
        self.flush()
        self.stop_event.set()
        self.worker.join(timeout=2.0)

    def _segment(self):
        """Finalize the current phrase when speech is followed by a real pause.

        Returns True when a final was emitted (window reset), False otherwise.
        """
        from faster_whisper.vad import get_speech_timestamps

        speech = get_speech_timestamps(self.window, self._vad_options, self.target_rate)
        if not speech:
            self._last_vad_metrics = self._vad_metrics(speech, last_speech_end=0)
            # Drop accumulated non-speech so leading silence never piles up.
            if self.window.size >= self.min_silence_samples:
                self.window_start_sample += self.window.size
                self.window = self.window[:0]
                self.samples_since_partial = 0
            return False

        last_speech_end = int(speech[-1]["end"])
        trailing_silence = self.window.size - last_speech_end
        self._last_vad_metrics = self._vad_metrics(speech, last_speech_end)

        if trailing_silence >= self.min_silence_samples:
            # Cut at the end of speech so the pause (not a word) splits the phrase.
            self._finalize_upto(last_speech_end,
                                reason="silence",
                                trailing_silence_samples=trailing_silence,
                                vad_metrics=self._last_vad_metrics)
            return True

        if self.window.size >= self.max_window_samples:
            # Speaker never paused; force a cut so latency and cost stay bounded.
            self._finalize_upto(self.window.size,
                                reason="max_window",
                                vad_metrics=self._last_vad_metrics)
            return True

        return False

    def _vad_metrics(self, speech, last_speech_end):
        import numpy as np

        window_samples = int(self.window.size)
        speech_samples = sum(max(0, int(item["end"]) - int(item["start"])) for item in speech)
        trailing_samples = max(0, window_samples - int(last_speech_end))
        tail_count = min(window_samples, max(self.min_silence_samples, 1))
        tail = self.window[-tail_count:] if tail_count > 0 else self.window[:0]

        metrics = {
            "vad_segment_count": len(speech),
            "vad_speech_ratio": round(speech_samples / window_samples, 3) if window_samples > 0 else 0.0,
            "vad_window_sec": round(window_samples / self.target_rate, 3),
            "vad_trailing_silence_sec": round(trailing_samples / self.target_rate, 3),
            "window_rms_dbfs": self._dbfs_float(np, self.window),
            "tail_rms_dbfs": self._dbfs_float(np, tail),
        }
        if speech:
            metrics["vad_last_speech_end_sec"] = round(
                (self.window_start_sample + int(last_speech_end)) / self.target_rate,
                3,
            )
        return metrics

    @staticmethod
    def _dbfs_float(np, samples):
        if samples.size == 0:
            return -120.0
        values = samples.astype(np.float64, copy=False)
        rms = float(np.sqrt(np.mean(values * values)))
        if rms <= 1e-12:
            return -120.0
        return round(max(-120.0, 20.0 * np.log10(rms)), 1)

    def _apply_gain(self, np, samples):
        if self.gain > 0.0:
            return np.clip(samples * self.gain, -1.0, 1.0)

        if samples.size == 0:
            return samples

        # Auto: track the recent peak (slow decay) and apply a single uniform gain
        # toward a target peak. Uniform gain keeps VAD boundaries stable; the slow
        # decay adapts to the capture level without chasing per-slice transients.
        absmax = float(np.max(np.abs(samples)))
        if absmax > 0.003:
            self._auto_peak = max(absmax, self._auto_peak * 0.999)
        if self._auto_peak <= 1e-4:
            return samples
        gain = min(0.45 / self._auto_peak, 100.0)
        return np.clip(samples * gain, -1.0, 1.0)

    def _emit_partial(self):
        chunk = self.window.copy()
        start_sample = self.window_start_sample
        end_sample = start_sample + chunk.size
        self.samples_since_partial = 0
        self._queue_job(chunk,
                        start_sample,
                        end_sample,
                        is_final=False,
                        reason="partial_tick",
                        trailing_silence_samples=0,
                        vad_metrics=self._last_vad_metrics)

    def _finalize_upto(self, count, reason, trailing_silence_samples=0, vad_metrics=None):
        chunk = self.window[:count].copy()
        start_sample = self.window_start_sample
        end_sample = start_sample + count
        # Keep whatever follows the cut (usually trailing silence) as the next window.
        self.window = self.window[count:].copy()
        self.window_start_sample = end_sample
        self.samples_since_partial = 0
        self._queue_job(chunk,
                        start_sample,
                        end_sample,
                        is_final=True,
                        reason=reason,
                        trailing_silence_samples=trailing_silence_samples,
                        vad_metrics=vad_metrics)

    def _queue_job(
        self,
        chunk,
        start_sample,
        end_sample,
        is_final,
        reason,
        trailing_silence_samples,
        vad_metrics=None,
    ):
        queued_at = time.monotonic()
        job = {
            "job_id": self._job_seq,
            "chunk": chunk,
            "start_sample": start_sample,
            "end_sample": end_sample,
            "is_final": is_final,
            "reason": reason,
            "trailing_silence_samples": trailing_silence_samples,
            "vad_metrics": dict(vad_metrics or {}),
            "queued_at": queued_at,
            "audio_end_monotonic": self._audio_end_monotonic(end_sample),
        }
        self._job_seq += 1
        with self._queue_lock:
            if not is_final:
                if self._partial_jobs_outstanding > 0:
                    self._dropped_jobs += 1
                    print("stt partial backpressure: dropping newer partial", flush=True)
                    return
                try:
                    self.pending_chunks.put_nowait(job)
                except queue.Full:
                    self._dropped_jobs += 1
                    print("stt partial backpressure: dropping partial behind finals", flush=True)
                    return
                self._partial_jobs_outstanding += 1
                return

            self._drop_pending_partials_locked()

        # Finals are not disposable. If the queue is full of finals, wait for the
        # worker instead of dropping a transcript segment.
        self.pending_chunks.put(job)

    def _drop_pending_partials_locked(self):
        kept = []
        dropped = 0

        while True:
            try:
                job = self.pending_chunks.get_nowait()
            except queue.Empty:
                break

            self.pending_chunks.task_done()
            if job.get("is_final", False):
                kept.append(job)
                continue

            dropped += 1
            self._dropped_jobs += 1
            self._partial_jobs_outstanding = max(0, self._partial_jobs_outstanding - 1)

        for job in kept:
            self.pending_chunks.put_nowait(job)

        if dropped > 0:
            print(f"stt final priority: dropped {dropped} queued partial(s)", flush=True)

    def _finish_job(self, job):
        if job.get("is_final", False):
            return

        with self._queue_lock:
            self._partial_jobs_outstanding = max(0, self._partial_jobs_outstanding - 1)

    def _audio_end_monotonic(self, end_sample):
        if (self._audio_start_monotonic is None) or (not self.realtime):
            return None
        return self._audio_start_monotonic + (end_sample / self.target_rate)

    def _worker_main(self):
        while not self.stop_event.is_set():
            try:
                job = self.pending_chunks.get(timeout=0.1)
            except queue.Empty:
                continue

            try:
                self._transcribe(job)
            finally:
                self._finish_job(job)
                self.pending_chunks.task_done()

    def _transcribe(self, job):
        chunk = job["chunk"]
        start_sample = job["start_sample"]
        end_sample = job["end_sample"]
        is_final = job["is_final"]
        start_sec = start_sample / self.target_rate
        end_sec = end_sample / self.target_rate
        queue_wait = time.monotonic() - job["queued_at"]

        t0 = time.monotonic()
        event = self.engine.transcribe_chunk(chunk, start_sec, end_sec, is_final)
        elapsed = time.monotonic() - t0
        kind = "final" if is_final else "partial"
        if event is None:
            print(
                f"stt empty {kind} t={start_sec:.3f}..{end_sec:.3f}s dt={elapsed:.3f}s",
                flush=True,
            )
            return

        emitted_at = time.monotonic()
        self._annotate_event(event, job, queue_wait, elapsed, emitted_at)
        filtered_events = self.partial_filter.handle_event(event)
        if not filtered_events:
            print(f"stt suppressed unstable {kind} dt={elapsed:.3f}s", flush=True)
            return

        for filtered_event in filtered_events:
            print(
                f"stt inference {kind} dt={elapsed:.3f}s "
                f"queue={queue_wait:.3f}s reason={job['reason']}",
                flush=True,
            )
            self.sink.handle_event(filtered_event)

    def _annotate_event(self, event, job, queue_wait, infer_sec, emitted_at):
        event["chunk_sec"] = round(event["end_sec"] - event["start_sec"], 3)
        event["segment_reason"] = job["reason"]
        event["queue_wait_sec"] = round(queue_wait, 3)
        event["infer_sec"] = round(infer_sec, 3)
        event["stt_wall_sec"] = round(queue_wait + infer_sec, 3)
        event["queue_depth_after_get"] = self.pending_chunks.qsize()
        event["dropped_audio_jobs"] = self._dropped_jobs
        event["partial_jobs_outstanding"] = self._partial_jobs_outstanding
        event["job_id"] = job["job_id"]
        event["trailing_silence_sec"] = round(
            job["trailing_silence_samples"] / self.target_rate,
            3,
        )
        for key, value in job.get("vad_metrics", {}).items():
            event[key] = value
        if job["audio_end_monotonic"] is not None:
            event["emit_lag_sec"] = round(emitted_at - job["audio_end_monotonic"], 3)
        for key, value in self.run_config.items():
            event[key] = value


class TcpAudioReceiver:
    def __init__(self, host, port, transcriber, save_wav=None, lossless_live=False):
        self.host = host
        self.port = port
        self.transcriber = transcriber
        self.save_wav = save_wav
        self.lossless_live = lossless_live
        self._wav = None
        self._processing_error = None

    def run(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((self.host, self.port))
            server.listen(1)
            print(f"listening for board audio on {self.host}:{self.port}", flush=True)

            conn, addr = server.accept()
            with conn:
                print(f"connected from {addr[0]}:{addr[1]}", flush=True)
                self._receive_stream(conn)

    def _receive_stream(self, conn):
        magic = recv_exact(conn, len(STREAM_MAGIC))
        if magic != STREAM_MAGIC:
            raise RuntimeError(f"bad stream magic: {magic!r}")

        header = recv_exact(conn, struct.calcsize(STREAM_HEADER))
        rate, channels, fmt, chunk_ms, samples_per_chunk, bytes_per_chunk = struct.unpack(
            STREAM_HEADER, header
        )
        validate_audio_format(rate, channels, fmt)
        self.transcriber.set_source_rate(rate)

        print(
            f"stream: {rate} Hz, {channels} ch, {chunk_ms} ms chunks, "
            f"{samples_per_chunk} samples/chunk",
            flush=True,
        )

        if self.save_wav:
            self._wav = wave.open(self.save_wav, "wb")
            self._wav.setnchannels(channels)
            self._wav.setsampwidth(EXPECTED_SAMPLE_WIDTH)
            self._wav.setframerate(rate)
            print(f"saving exact board audio to {self.save_wav}", flush=True)

        if self.lossless_live:
            print("live audio buffering: lossless mode enabled", flush=True)

        try:
            while True:
                if self.lossless_live:
                    self._receive_stream_lossless(conn, rate, channels, fmt, bytes_per_chunk)
                    break
                self._receive_chunk(conn, rate, channels, fmt, bytes_per_chunk)
        except EOFError:
            print("audio connection closed", flush=True)
            self.transcriber.flush()
        finally:
            if self._wav is not None:
                self._wav.close()
                self._wav = None

    def _receive_stream_lossless(self, conn, rate, channels, fmt, bytes_per_chunk):
        frames = queue.Queue()
        stop_marker = object()

        def process_frames():
            try:
                while True:
                    payload = frames.get()
                    try:
                        if payload is stop_marker:
                            return
                        self.transcriber.push_pcm(payload)
                    finally:
                        frames.task_done()
            except Exception as exc:  # pragma: no cover - re-raised on reader thread
                self._processing_error = exc

        worker = threading.Thread(target=process_frames, daemon=True)
        worker.start()

        try:
            while True:
                payload = self._receive_chunk(conn, rate, channels, fmt, bytes_per_chunk)
                frames.put(payload)
                if self._processing_error is not None:
                    raise self._processing_error
        except EOFError:
            frames.put(stop_marker)
            frames.join()
            worker.join(timeout=2.0)
            if self._processing_error is not None:
                raise self._processing_error
            raise

    def _receive_chunk(self, conn, rate, channels, fmt, bytes_per_chunk):
        chunk_header = recv_exact(conn, struct.calcsize(CHUNK_HEADER))
        seq, timestamp_ns, payload_bytes, dropped, chunk_rate, chunk_channels, chunk_fmt = (
            struct.unpack(CHUNK_HEADER, chunk_header)
        )
        if payload_bytes > bytes_per_chunk:
            raise RuntimeError(f"bad payload size: {payload_bytes}")
        if (chunk_rate, chunk_channels, chunk_fmt) != (rate, channels, fmt):
            raise RuntimeError("chunk format changed")

        payload = recv_exact(conn, payload_bytes)
        if seq % 50 == 0:
            seconds = timestamp_ns / 1_000_000_000.0
            print(f"audio seq={seq} t={seconds:.3f}s dropped={dropped}", flush=True)

        if self._wav is not None:
            self._wav.writeframes(payload)

        if not self.lossless_live:
            self.transcriber.push_pcm(payload)

        return payload


def validate_audio_format(rate, channels, fmt):
    if rate <= 0:
        raise RuntimeError(f"invalid audio rate: {rate}")
    if channels != EXPECTED_CHANNELS:
        raise RuntimeError(f"expected mono audio, got {channels} channels")
    if fmt != FORMAT_S16_LE:
        raise RuntimeError(f"unsupported audio format: {fmt}")


def run_wav(path, transcriber):
    with wave.open(path, "rb") as wav:
        rate = wav.getframerate()
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        if sample_width != EXPECTED_SAMPLE_WIDTH:
            raise RuntimeError(f"expected 16-bit PCM WAV, got sample width {sample_width}")
        validate_audio_format(rate, channels, FORMAT_S16_LE)
        transcriber.set_source_rate(rate)

        frames_per_read = max(1, rate // 10)
        print(f"reading WAV {path}: {rate} Hz, {channels} ch", flush=True)
        while True:
            payload = wav.readframes(frames_per_read)
            if not payload:
                break
            transcriber.push_pcm(payload)

    transcriber.flush()


def run_audio_file(path, transcriber):
    """Transcribe any audio file (m4a/mp3/wav/...) offline via the same pipeline.

    Uses faster-whisper's decoder (PyAV) so no external ffmpeg is required, then
    streams the decoded audio through the chunk transcriber to mimic the live path.
    """
    import numpy as np
    from faster_whisper.audio import decode_audio

    audio = decode_audio(path, sampling_rate=WHISPER_RATE)
    transcriber.set_source_rate(WHISPER_RATE)
    print(f"decoded {path}: {audio.size / WHISPER_RATE:.1f} s at {WHISPER_RATE} Hz", flush=True)

    pcm = np.clip(audio * 32768.0, -32768.0, 32767.0).astype("<i2")
    step = max(1, WHISPER_RATE // 10)  # feed in ~100 ms slices like the live stream
    for start in range(0, pcm.size, step):
        transcriber.push_pcm(pcm[start : start + step].tobytes())

    transcriber.flush()


def build_sink(args):
    sinks = [ConsoleTranscriptSink()]
    if args.jsonl:
        sinks.append(JsonlTranscriptSink(args.jsonl))
    if args.send_subtitles:
        max_queue = 0 if args.lossless_live else 32
        sinks.append(TcpTranscriptSink(args.subtitle_host, args.subtitle_port, max_queue=max_queue))
    return CompositeTranscriptSink(sinks)


def build_run_config(args, offline):
    engine = "colab" if args.colab_url else "local"
    config = {
        "run_engine": engine,
        "config_max_window_sec": float(args.max_window_sec),
        "config_min_silence_sec": float(args.min_silence_sec),
        "config_partial_sec": float(args.partial_sec),
        "config_partial_agreement": int(args.partial_agreement),
        "config_beam_size": int(args.beam_size),
        "config_vad_filter": bool(args.vad_filter),
        "config_lossless_live": bool(args.lossless_live),
        "config_realtime": not offline,
        "config_gain": float(args.gain),
        "config_partial_backpressure": True,
    }
    if args.colab_url:
        config["config_model"] = "colab"
    else:
        config["config_model"] = args.model
        config["config_device"] = args.device
        config["config_compute_type"] = args.compute_type
        config["config_cpu_threads"] = int(args.cpu_threads)
    return config


def print_run_config(config):
    shown = " ".join(f"{key}={value}" for key, value in sorted(config.items()))
    print(f"stt run config: {shown}", flush=True)


def parse_args():
    parser = argparse.ArgumentParser(description="Receive PCM audio and run faster-whisper STT")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--wav", help="transcribe a 16 kHz mono S16_LE WAV instead of TCP")
    parser.add_argument(
        "--audio-file",
        help="transcribe any audio file (m4a/mp3/wav/...) offline instead of TCP",
    )
    parser.add_argument(
        "--max-window-sec",
        type=float,
        default=8.0,
        help="force a cut if speech runs this long without a pause",
    )
    parser.add_argument(
        "--min-silence-sec",
        type=float,
        default=0.5,
        help="a silence at least this long finalizes the current phrase (VAD segmentation)",
    )
    # Deprecated alias kept so older commands/launchers don't break.
    parser.add_argument("--chunk-sec", type=float, default=None, help=argparse.SUPPRESS)
    parser.add_argument(
        "--partial-sec",
        type=float,
        default=0.7,
        help="emit a partial (is_final=false) hypothesis every N seconds; 0 disables partials",
    )
    parser.add_argument(
        "--partial-agreement",
        type=int,
        default=1,
        help="require N consecutive partials to share a word prefix before emitting; 1 disables",
    )
    parser.add_argument(
        "--gain",
        type=float,
        default=0.0,
        help="fixed input gain multiplier; 0 (default) auto-normalizes each phrase",
    )
    parser.add_argument("--model", default="small")
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--compute-type", default="int8")
    parser.add_argument(
        "--cpu-threads",
        type=int,
        default=0,
        help="CPU threads for faster-whisper/CTranslate2; 0 lets the backend choose",
    )
    parser.add_argument("--language", default="es")
    parser.add_argument("--beam-size", type=int, default=5)
    parser.add_argument("--vad-filter", action="store_true")
    parser.add_argument(
        "--colab-url",
        help="use remote Colab inference server instead of local Faster Whisper (e.g., https://xxx.ngrok.io)",
    )
    parser.add_argument(
        "--lossless-live",
        action="store_true",
        help="buffer live TCP audio/transcript work instead of dropping when STT falls behind",
    )
    parser.add_argument("--jsonl", help="write transcript events as JSON Lines")
    parser.add_argument(
        "--save-wav",
        help="save the exact PCM received from the board to a WAV (live TCP mode only)",
    )
    parser.add_argument("--send-subtitles", action="store_true")
    parser.add_argument("--subtitle-host", default="192.168.1.10")
    parser.add_argument("--subtitle-port", type=int, default=5001)
    args = parser.parse_args()

    if args.chunk_sec is not None:
        # Map the deprecated flag onto the forced-cut cap.
        args.max_window_sec = args.chunk_sec
    if args.max_window_sec <= 0.0:
        parser.error("--max-window-sec must be positive")
    if args.min_silence_sec < 0.0:
        parser.error("--min-silence-sec must be zero or positive")
    if args.partial_sec < 0.0:
        parser.error("--partial-sec must be zero or positive")
    if args.partial_agreement < 1:
        parser.error("--partial-agreement must be positive")
    if args.gain < 0.0:
        parser.error("--gain must be zero (auto) or positive")
    if args.cpu_threads < 0:
        parser.error("--cpu-threads must be zero or positive")
    if args.partial_sec >= args.max_window_sec:
        print(
            "warning: --partial-sec >= --max-window-sec disables partial updates in practice",
            flush=True,
        )

    return args


def main():
    args = parse_args()
    sink = build_sink(args)
    transcriber = None

    try:
        if args.colab_url:
            engine = ColabWhisperEngine(
                colab_url=args.colab_url,
                language=args.language,
                beam_size=args.beam_size,
                vad_filter=args.vad_filter,
            )
        else:
            engine = FasterWhisperEngine(
                model_size=args.model,
                device=args.device,
                compute_type=args.compute_type,
                language=args.language,
                beam_size=args.beam_size,
                vad_filter=args.vad_filter,
                cpu_threads=args.cpu_threads,
            )
        offline = bool(args.audio_file) or bool(args.wav)
        run_config = build_run_config(args, offline)
        print_run_config(run_config)
        transcriber = ChunkTranscriber(
            engine,
            sink,
            WHISPER_RATE,
            args.max_window_sec,
            partial_sec=args.partial_sec,
            min_silence_sec=args.min_silence_sec,
            gain=args.gain,
            partial_agreement=args.partial_agreement,
            drop_oldest=not (offline or args.lossless_live),
            realtime=not offline,
            run_config=run_config,
        )

        if args.audio_file:
            run_audio_file(args.audio_file, transcriber)
        elif args.wav:
            run_wav(args.wav, transcriber)
        else:
            TcpAudioReceiver(
                args.host,
                args.port,
                transcriber,
                save_wav=args.save_wav,
                lossless_live=args.lossless_live,
            ).run()
    finally:
        if transcriber is not None:
            transcriber.close()
        sink.close()


if __name__ == "__main__":
    main()
