#!/usr/bin/env python3
"""Analyze a subtitle STT run and print a compact report.

Usage:
    python3 scripts/analyze_run.py [--jsonl logs/stt_events.jsonl] [--wav logs/board_audio.wav]
"""

import argparse
import json
import math
import struct
import sys
import wave
from collections import Counter
from pathlib import Path


SUBTITLE_CLEAR_TIMEOUT_MS = 5000
DEFAULT_STT_MAX_WINDOW_SEC = 4.0

HALLUCINATION_MARKERS = (
    "amara.org",
    "subtítulos realizados por",
    "subtitulos realizados por",
    "suscríbete al canal",
    "gracias por ver el video",
    "gracias por ver este video",
)

CONFIG_KEYS = (
    "run_engine",
    "config_model",
    "config_max_window_sec",
    "config_min_silence_sec",
    "config_partial_sec",
    "config_partial_agreement",
    "config_beam_size",
    "config_vad_filter",
    "config_lossless_live",
    "config_realtime",
    "config_gain",
    "config_partial_backpressure",
    "config_device",
    "config_compute_type",
    "config_cpu_threads",
    "config_transport",
)


def is_hallucination(text):
    lowered = text.lower()
    return any(marker in lowered for marker in HALLUCINATION_MARKERS)


def _dbfs(rms, full_scale=32768.0):
    if rms <= 0:
        return -math.inf
    return 20.0 * math.log10(rms / full_scale)


def _rms(samples_flat):
    if not samples_flat:
        return 0.0
    total = sum(sample * sample for sample in samples_flat)
    return math.sqrt(total / len(samples_flat))


def window_rms_dbfs(samples, framerate, window_ms=200):
    if not samples or framerate <= 0:
        return []

    window_samples = max(1, int(framerate * window_ms / 1000.0))
    values = []
    for start in range(0, len(samples), window_samples):
        chunk = samples[start : start + window_samples]
        if chunk:
            values.append(_dbfs(_rms(chunk)))
    return values


def percentile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[round((len(ordered) - 1) * q)]


def mean(values):
    if not values:
        return None
    return sum(values) / len(values)


def numeric_values(events, key):
    values = []
    for event in events:
        value = event.get(key)
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            values.append(float(value))
    return values


def print_header(title, verdict):
    print("=" * 60)
    print(f"{title}  [{verdict}]")
    print("=" * 60)


def print_stat_line(label, values, unit="s"):
    if not values:
        print(f"  {label:<18}: n/a")
        return
    suffix = unit
    print(
        f"  {label:<18}: n={len(values)} "
        f"p50={percentile(values, 0.50):.2f}{suffix} "
        f"p90={percentile(values, 0.90):.2f}{suffix} "
        f"max={max(values):.2f}{suffix} "
        f"mean={mean(values):.2f}{suffix}"
    )


def event_window_sec(event):
    if "start_sec" not in event or "end_sec" not in event:
        return None
    return float(event["end_sec"]) - float(event["start_sec"])


def analyze_wav(path):
    try:
        wf = wave.open(str(path), "rb")
    except Exception as exc:
        print(f"AUDIO: could not open {path}: {exc}")
        return

    nchannels = wf.getnchannels()
    sampwidth = wf.getsampwidth()
    framerate = wf.getframerate()
    nframes = wf.getnframes()
    duration_sec = nframes / framerate
    raw = wf.readframes(nframes)
    wf.close()

    if sampwidth != 2:
        print(f"AUDIO: unexpected sample width {sampwidth * 8}-bit; skipping level analysis")
        print(f"  duration: {duration_sec:.1f}s  channels: {nchannels}")
        return

    n_samples = len(raw) // 2
    samples = struct.unpack(f"<{n_samples}h", raw)
    if nchannels > 1:
        samples = samples[::nchannels]

    peak_abs = max(abs(sample) for sample in samples) if samples else 0
    peak_pct = 100.0 * peak_abs / 32767.0
    rms_db = _dbfs(_rms(samples))
    clipped = sum(1 for sample in samples if abs(sample) >= 32765)
    clipped_pct = 100.0 * clipped / len(samples) if samples else 0.0
    window_levels = window_rms_dbfs(samples, framerate)
    floor_db = percentile(window_levels, 0.10)
    median_db = percentile(window_levels, 0.50)
    loud_db = percentile(window_levels, 0.90)
    dynamic_range_db = (
        (loud_db - floor_db)
        if floor_db is not None and loud_db is not None and math.isfinite(floor_db)
        else None
    )

    if peak_pct < 5.0:
        verdict = "TOO QUIET"
    elif clipped_pct > 0.01:
        verdict = f"CLIPPING ({clipped_pct:.3f}% samples at ceiling)"
    elif (
        floor_db is not None
        and dynamic_range_db is not None
        and floor_db > -35.0
        and dynamic_range_db < 15.0
    ):
        verdict = "NOISY FLOOR"
    else:
        verdict = "OK"

    print_header("AUDIO", verdict)
    print(f"  duration : {duration_sec:.1f}s")
    print(f"  peak     : {peak_pct:.1f}% of full scale  ({peak_abs})")
    print(f"  RMS      : {rms_db:.1f} dBFS")
    if floor_db is not None and median_db is not None and loud_db is not None:
        print(
            f"  windows  : floor p10={floor_db:.1f} dBFS  "
            f"median={median_db:.1f} dBFS  loud p90={loud_db:.1f} dBFS"
        )
    if dynamic_range_db is not None:
        print(f"  range    : p90-p10={dynamic_range_db:.1f} dB")
        if floor_db is not None and floor_db > -35.0:
            print("  NOTE: quietest windows are loud; VAD may never see real silence")
    print()


def run_config(events):
    for event in events:
        config = {key: event[key] for key in CONFIG_KEYS if key in event}
        if config:
            return config
    return {}


def analyze_config(events):
    config = run_config(events)
    if not config:
        print_header("CONFIG", "legacy log")
        print("  no instrumented run config in JSONL")
        print()
        return

    print_header("CONFIG", "instrumented")
    print(f"  engine       : {config.get('run_engine', 'unknown')}")
    print(f"  model        : {config.get('config_model', 'unknown')}")
    print(f"  max window   : {config.get('config_max_window_sec')}s")
    print(f"  min silence  : {config.get('config_min_silence_sec')}s")
    print(f"  partial      : every {config.get('config_partial_sec')}s")
    print(f"  agreement    : {config.get('config_partial_agreement')}")
    print(f"  beam         : {config.get('config_beam_size')}")
    print(f"  VAD/filter   : {config.get('config_vad_filter')}")
    print(f"  lossless     : {config.get('config_lossless_live')}")
    print(f"  partial bp   : {config.get('config_partial_backpressure', False)}")
    if "config_transport" in config:
        print(f"  transport    : {config.get('config_transport')}")
    print()


def analyze_events(events):
    total = len(events)
    finals = [event for event in events if event.get("is_final")]
    partials = [event for event in events if not event.get("is_final")]
    hallucinations = [event for event in events if is_hallucination(event.get("text", ""))]

    seq_issues = []
    seen_seqs = set()
    prev_seq = None
    for event in events:
        seq = event.get("seq")
        if seq is None:
            continue
        if seq in seen_seqs:
            seq_issues.append(f"dup seq={seq}")
        seen_seqs.add(seq)
        if prev_seq is not None and seq <= prev_seq:
            seq_issues.append(f"non-monotonic seq={seq} after seq={prev_seq}")
        prev_seq = seq

    seq_verdict = "OK" if not seq_issues else f"ISSUES: {'; '.join(seq_issues[:5])}"
    reasons = Counter(event.get("segment_reason", "unknown") for event in events)
    dropped = max((int(event.get("dropped_audio_jobs", 0)) for event in events), default=0)

    print_header("EVENTS", f"seq {seq_verdict}")
    print(f"  total     : {total}  (finals={len(finals)}, partials={len(partials)})")
    print(f"  reasons   : {dict(sorted(reasons.items()))}")
    print(f"  dropped   : {dropped} audio job(s)")
    if hallucinations:
        print(f"  hallucin. : {len(hallucinations)} event(s) flagged")
        for event in hallucinations[:3]:
            print(f"    seq={event.get('seq')}  {event.get('text', '')[:60]!r}")
    else:
        print("  hallucin. : 0")
    print()


def partials_before_finals(events):
    finals = [event for event in events if event.get("is_final")]
    counts = []
    previous_final_seq = -1
    for final in finals:
        final_seq = final.get("seq", -1)
        counts.append(
            sum(
                1
                for event in events
                if not event.get("is_final")
                and previous_final_seq < event.get("seq", -1) < final_seq
            )
        )
        previous_final_seq = final_seq
    return counts


def analyze_segmentation(events):
    finals = [event for event in events if event.get("is_final")]
    partials = [event for event in events if not event.get("is_final")]
    final_windows = [value for value in (event_window_sec(event) for event in finals) if value is not None]
    partial_windows = [value for value in (event_window_sec(event) for event in partials) if value is not None]
    config = run_config(events)
    max_window = float(config.get("config_max_window_sec", DEFAULT_STT_MAX_WINDOW_SEC))
    cap_hits = sum(1 for value in final_windows if abs(value - max_window) <= 0.06)
    cap_ratio = (cap_hits / len(final_windows)) if final_windows else 0.0

    notes = []
    if cap_ratio >= 0.60:
        notes.append(f"{cap_hits}/{len(final_windows)} finals hit max_window={max_window:.2f}s")
    if partial_windows and min(partial_windows) > 1.2:
        notes.append(f"first visible partial window starts late-ish ({min(partial_windows):.2f}s)")

    verdict = "OK" if not notes else "CAP-LIMITED"
    print_header("SEGMENTATION", verdict)
    print_stat_line("final windows", final_windows)
    print_stat_line("partial windows", partial_windows)
    counts = partials_before_finals(events)
    if counts:
        print(
            f"  partials/final    : p50={percentile(counts, 0.50):.0f} "
            f"max={max(counts)} counts={dict(sorted(Counter(counts).items()))}"
        )
    if events:
        first = events[0]
        first_window = event_window_sec(first)
        if first_window is not None:
            kind = "final" if first.get("is_final") else "partial"
            print(f"  first event      : {kind} window={first_window:.2f}s seq={first.get('seq')}")
    if finals:
        first_final_window = event_window_sec(finals[0])
        if first_final_window is not None:
            print(f"  first final      : window={first_final_window:.2f}s seq={finals[0].get('seq')}")
    for note in notes:
        print(f"  NOTE: {note}")
    print()


def analyze_vad(events):
    vad_events = [
        event
        for event in events
        if "vad_speech_ratio" in event
        or "vad_segment_count" in event
        or "tail_rms_dbfs" in event
    ]
    if not vad_events:
        print_header("VAD", "not instrumented")
        print("  no VAD diagnostics in JSONL")
        print()
        return

    finals = [event for event in vad_events if event.get("is_final")]
    silence_finals = [event for event in finals if event.get("segment_reason") == "silence"]
    cap_finals = [event for event in finals if event.get("segment_reason") == "max_window"]
    speech_ratio = numeric_values(vad_events, "vad_speech_ratio")
    segment_counts = numeric_values(vad_events, "vad_segment_count")
    vad_trailing = numeric_values(vad_events, "vad_trailing_silence_sec")
    trailing = numeric_values(vad_events, "trailing_silence_sec")
    window_rms = numeric_values(vad_events, "window_rms_dbfs")
    tail_rms = numeric_values(vad_events, "tail_rms_dbfs")
    cap_speech_ratio = numeric_values(cap_finals, "vad_speech_ratio")
    silence_trailing = numeric_values(silence_finals, "vad_trailing_silence_sec")

    notes = []
    if cap_finals and len(cap_finals) >= len(silence_finals):
        notes.append(f"{len(cap_finals)}/{len(finals)} finals still reached max_window")
    if speech_ratio and percentile(speech_ratio, 0.50) >= 0.85:
        notes.append("median VAD speech ratio is high; VAD may be seeing near-continuous speech")
    if tail_rms and percentile(tail_rms, 0.50) > -35.0:
        notes.append(f"tail RMS is noisy (p50={percentile(tail_rms, 0.50):.1f} dBFS)")
    if silence_finals:
        notes.append(f"{len(silence_finals)} final(s) were cut by VAD silence")

    if not notes:
        verdict = "OK"
    elif silence_finals and cap_finals:
        verdict = "MIXED"
    elif cap_finals:
        verdict = "CAP-LIMITED"
    else:
        verdict = "ATTENTION"

    print_header("VAD", verdict)
    print_stat_line("segments/event", segment_counts, unit="")
    print_stat_line("speech ratio", speech_ratio, unit="")
    print_stat_line("vad trailing", vad_trailing)
    print_stat_line("job trailing", trailing)
    print_stat_line("window RMS", window_rms, unit=" dBFS")
    print_stat_line("tail RMS", tail_rms, unit=" dBFS")
    print(f"  final reasons     : silence={len(silence_finals)} max_window={len(cap_finals)} total={len(finals)}")
    if cap_speech_ratio:
        print_stat_line("cap speech ratio", cap_speech_ratio, unit="")
    if silence_trailing:
        print_stat_line("silence trailing", silence_trailing)
    for note in notes:
        print(f"  NOTE: {note}")
    print()


def analyze_pipeline(events):
    queue_wait = numeric_values(events, "queue_wait_sec")
    infer = numeric_values(events, "infer_sec")
    remote_infer = numeric_values(events, "remote_infer_sec")
    wall = numeric_values(events, "stt_wall_sec")
    emit_lag = numeric_values(events, "emit_lag_sec")
    server_queue = numeric_values(events, "server_queue_sec")
    gpu_infer = numeric_values(events, "gpu_infer_sec")
    server_emit_lag = numeric_values(events, "server_emit_lag_sec")
    bridge_receive_lag = numeric_values(events, "bridge_receive_lag_sec")
    audio_buffer = numeric_values(events, "audio_buffer_sec")

    if not (
        queue_wait
        or infer
        or wall
        or emit_lag
        or server_queue
        or gpu_infer
        or server_emit_lag
        or bridge_receive_lag
    ):
        print_header("PIPELINE", "legacy log")
        print("  no queue/inference/lag metrics in JSONL")
        print()
        return

    notes = []
    if queue_wait and max(queue_wait) > 0.5:
        notes.append(f"queue backlog reached {max(queue_wait):.2f}s")
    if emit_lag and percentile(emit_lag, 0.90) > 2.0:
        notes.append(f"p90 emit lag is {percentile(emit_lag, 0.90):.2f}s")
    if server_emit_lag and min(server_emit_lag) < -0.05:
        notes.append(f"server emit lag still has negative samples (min={min(server_emit_lag):.2f}s)")
    if bridge_receive_lag and min(bridge_receive_lag) < -0.05:
        notes.append(f"bridge receive lag still has negative samples (min={min(bridge_receive_lag):.2f}s)")

    verdict = "OK" if not notes else "ATTENTION"
    print_header("PIPELINE", verdict)
    print_stat_line("queue wait", queue_wait)
    print_stat_line("local/total infer", infer)
    if remote_infer:
        print_stat_line("remote infer", remote_infer)
    print_stat_line("queue+infer", wall)
    print_stat_line("emit lag", emit_lag)
    if server_queue or gpu_infer or server_emit_lag or bridge_receive_lag:
        print("  -- streaming breakdown --")
        print_stat_line("audio buffer", audio_buffer)
        print_stat_line("server queue", server_queue)
        print_stat_line("GPU infer", gpu_infer)
        print_stat_line("server emit lag", server_emit_lag)
        print_stat_line("bridge recv lag", bridge_receive_lag)
    for note in notes:
        print(f"  NOTE: {note}")
    print()


def analyze_display_cadence(events):
    timed = [
        event
        for event in events
        if isinstance(event.get("bridge_received_monotonic"), (int, float))
    ]
    if len(timed) < 2:
        print_header("DISPLAY", "not enough receive timestamps")
        print("  no bridge receive timestamps available")
        print()
        return

    timed = sorted(timed, key=lambda event: event.get("seq", 0))
    intervals = [
        float(b["bridge_received_monotonic"]) - float(a["bridge_received_monotonic"])
        for a, b in zip(timed, timed[1:])
    ]
    partial_intervals = [
        interval
        for event, interval in zip(timed, intervals)
        if not event.get("is_final")
    ]
    final_intervals = [
        interval
        for event, interval in zip(timed, intervals)
        if event.get("is_final")
    ]
    short_all = sum(1 for interval in intervals if interval < 1.5)
    short_partials = sum(1 for interval in partial_intervals if interval < 1.5)
    short_finals = sum(1 for interval in final_intervals if interval < 1.5)

    verdict = "OK" if short_all == 0 else "FAST UPDATES"
    print_header("DISPLAY", verdict)
    print_stat_line("event spacing", intervals)
    print_stat_line("partial spacing", partial_intervals)
    print_stat_line("final spacing", final_intervals)
    print(
        f"  <1.5s visible    : all={short_all}/{len(intervals)} "
        f"partials={short_partials}/{len(partial_intervals)} "
        f"finals={short_finals}/{len(final_intervals)}"
    )
    if short_all > 0:
        print("  NOTE: events are replacing text faster than a 1.5s readability target")
    print()


def analyze_timing(events):
    finals = sorted(
        [event for event in events if event.get("is_final") and "end_sec" in event],
        key=lambda event: event.get("seq", 0),
    )
    if len(finals) < 2:
        print_header("TIMING", "not enough finals")
        print()
        return

    gaps = [b["start_sec"] - a["end_sec"] for a, b in zip(finals, finals[1:])]
    run_start = finals[0]["start_sec"]
    run_end = finals[-1]["end_sec"]
    max_gap = max(gaps)
    mean_gap = sum(gaps) / len(gaps)

    print_header("TIMING", "OK")
    print(f"  finals    : {len(finals)}")
    print(f"  run span  : {run_end - run_start:.1f}s  ({run_start:.1f}s -> {run_end:.1f}s)")
    print(f"  gap max   : {max_gap:.2f}s")
    print(f"  gap mean  : {mean_gap:.2f}s")
    if max_gap > (SUBTITLE_CLEAR_TIMEOUT_MS / 1000.0):
        print("  NOTE: long silence detected; this is not necessarily a firmware hold-time issue")
    print()


def transcript_sample(events, head=200, tail=200):
    finals = sorted(
        [event for event in events if event.get("is_final")],
        key=lambda event: event.get("seq", 0),
    )
    full = " ".join(event.get("text", "") for event in finals).strip()
    if not full:
        print_header("TRANSCRIPT", "no final events")
        print()
        return

    print_header("TRANSCRIPT", f"{len(full)} chars total from {len(finals)} finals")
    if len(full) <= head + tail:
        print(f"  {full}")
    else:
        print(f"  ...HEAD: {full[:head]}")
        print(f"  ...TAIL: {full[-tail:]}")
    print()


def load_events(path):
    events = []
    with open(path, encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                parsed = json.loads(line)
            except json.JSONDecodeError as exc:
                print(f"  [WARN] line {lineno}: JSON parse error: {exc}", file=sys.stderr)
                continue
            if isinstance(parsed, dict):
                events.append(parsed)
            else:
                print(f"  [WARN] line {lineno}: expected JSON object", file=sys.stderr)
    return events


def main():
    parser = argparse.ArgumentParser(
        description="Analyze a subtitle STT run; print a compact report."
    )
    parser.add_argument(
        "--jsonl",
        default="logs/stt_events.jsonl",
        help="NDJSON events file (default: logs/stt_events.jsonl)",
    )
    parser.add_argument(
        "--wav",
        default="logs/board_audio.wav",
        help="Board PCM wav file (default: logs/board_audio.wav)",
    )
    args = parser.parse_args()

    wav_path = Path(args.wav)
    jsonl_path = Path(args.jsonl)

    print()
    if wav_path.exists():
        analyze_wav(wav_path)
    else:
        print(f"[AUDIO section skipped: {wav_path} not found]\n")

    if not jsonl_path.exists():
        print(f"[EVENTS/TIMING/TRANSCRIPT sections skipped: {jsonl_path} not found]\n")
        return

    events = load_events(jsonl_path)
    if not events:
        print("[EVENTS/TIMING/TRANSCRIPT sections skipped: no events]\n")
        return

    analyze_config(events)
    analyze_events(events)
    analyze_segmentation(events)
    analyze_vad(events)
    analyze_pipeline(events)
    analyze_display_cadence(events)
    analyze_timing(events)
    transcript_sample(events)


if __name__ == "__main__":
    main()
