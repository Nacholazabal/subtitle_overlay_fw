#!/usr/bin/env python3
"""Analyze a subtitle STT run and print a compact report.

Usage:
    python scripts/analyze_run.py [--jsonl logs/stt_events.jsonl] [--wav logs/board_audio.wav]
"""

import argparse
import json
import math
import struct
import sys
import wave
from pathlib import Path

# ── tunables (from firmware / run_stt_windows.sh) ───────────────────────────
SUBTITLE_CLEAR_TIMEOUT_MS = 5000
STT_MAX_WINDOW_SEC = 4.0
STT_MIN_SILENCE_SEC = 0.5

# Mirror of HALLUCINATION_MARKERS / is_hallucination from stt_receiver.py
HALLUCINATION_MARKERS = (
    "amara.org",
    "subtítulos realizados por",
    "subtitulos realizados por",
    "suscríbete al canal",
    "gracias por ver el video",
    "gracias por ver este video",
)


def is_hallucination(text):
    lowered = text.lower()
    return any(m in lowered for m in HALLUCINATION_MARKERS)


# ── helpers ──────────────────────────────────────────────────────────────────

def _dbfs(rms, full_scale=32768.0):
    if rms <= 0:
        return -math.inf
    return 20.0 * math.log10(rms / full_scale)


def _rms(samples_flat):
    n = len(samples_flat)
    if n == 0:
        return 0.0
    total = sum(s * s for s in samples_flat)
    return math.sqrt(total / n)


# ── section 1: audio ─────────────────────────────────────────────────────────

def analyze_wav(path):
    try:
        wf = wave.open(str(path), "rb")
    except Exception as e:
        print(f"AUDIO: could not open {path}: {e}")
        return

    nchannels = wf.getnchannels()
    sampwidth = wf.getsampwidth()
    framerate = wf.getframerate()
    nframes = wf.getnframes()
    duration_sec = nframes / framerate

    raw = wf.readframes(nframes)
    wf.close()

    # Decode S16_LE (only format expected; handle gracefully if not)
    if sampwidth != 2:
        print(f"AUDIO: unexpected sample width {sampwidth*8}-bit; skipping level analysis")
        print(f"  duration: {duration_sec:.1f}s  channels: {nchannels}")
        return

    n_samples = len(raw) // 2
    samples = struct.unpack(f"<{n_samples}h", raw)

    # Flatten to mono if needed
    if nchannels > 1:
        samples = samples[::nchannels]

    peak_abs = max(abs(s) for s in samples) if samples else 0
    peak_pct = 100.0 * peak_abs / 32767.0
    rms_val = _rms(samples)
    rms_db = _dbfs(rms_val)

    # Clipping: count samples at or near ceiling
    ceiling = 32767
    clipped = sum(1 for s in samples if abs(s) >= ceiling - 2)
    clipped_pct = 100.0 * clipped / len(samples) if samples else 0

    if peak_pct < 5.0:
        verdict = "TOO QUIET (raise board gain / AGC)"
    elif clipped_pct > 0.01:
        verdict = f"CLIPPING ({clipped_pct:.3f}% samples at ceiling)"
    else:
        verdict = "OK"

    print("=" * 60)
    print(f"AUDIO  [{verdict}]")
    print("=" * 60)
    print(f"  duration : {duration_sec:.1f}s")
    print(f"  peak     : {peak_pct:.1f}% of full scale  ({peak_abs})")
    print(f"  RMS      : {rms_db:.1f} dBFS")
    print()


# ── section 2: events ────────────────────────────────────────────────────────

def analyze_events(events):
    total = len(events)
    finals = [e for e in events if e.get("is_final")]
    partials = [e for e in events if not e.get("is_final")]
    hallucinations = [e for e in events if is_hallucination(e.get("text", ""))]

    # Seq monotonicity
    seq_issues = []
    seen_seqs = {}
    prev_seq = None
    for e in events:
        s = e.get("seq")
        if s is None:
            continue
        if s in seen_seqs:
            seq_issues.append(f"dup seq={s}")
        seen_seqs[s] = True
        if prev_seq is not None and s <= prev_seq:
            seq_issues.append(f"non-monotonic seq={s} after seq={prev_seq}")
        prev_seq = s

    seq_verdict = "OK" if not seq_issues else f"ISSUES: {'; '.join(seq_issues[:5])}"

    print("=" * 60)
    print(f"EVENTS  [seq {seq_verdict}]")
    print("=" * 60)
    print(f"  total     : {total}  (finals={len(finals)}, partials={len(partials)})")
    if hallucinations:
        print(f"  hallucin. : {len(hallucinations)} event(s) flagged")
        for h in hallucinations[:3]:
            print(f"    seq={h.get('seq')}  {h.get('text','')[:60]!r}")
    else:
        print(f"  hallucin. : 0")
    print()


# ── section 3: timing ────────────────────────────────────────────────────────

def analyze_timing(events):
    finals = sorted(
        [e for e in events if e.get("is_final") and "end_sec" in e],
        key=lambda e: e["seq"],
    )

    if len(finals) < 2:
        print("=" * 60)
        print("TIMING  [not enough finals to compute gaps]")
        print("=" * 60)
        print()
        return

    gaps = []
    for a, b in zip(finals, finals[1:]):
        gap = b["start_sec"] - a["end_sec"]
        gaps.append(gap)

    max_gap = max(gaps)
    mean_gap = sum(gaps) / len(gaps)

    # Verdict
    notes = []
    if max_gap >= STT_MAX_WINDOW_SEC:
        notes.append(
            f"max gap ({max_gap:.1f}s) >= STT_MAX_WINDOW_SEC ({STT_MAX_WINDOW_SEC}s) "
            f"-- speaker never pauses; windows hitting the cap"
        )

    # Recommend clear timeout > max gap between finals
    # (so the board does not clear mid-speech)
    suggested_clear_ms = (math.ceil(max_gap) + 1) * 1000
    if suggested_clear_ms > SUBTITLE_CLEAR_TIMEOUT_MS:
        notes.append(
            f"SUBTITLE_CLEAR_TIMEOUT_MS should be > {max_gap:.1f}s "
            f"-- suggest {suggested_clear_ms} ms (currently {SUBTITLE_CLEAR_TIMEOUT_MS} ms)"
        )

    run_start = finals[0]["start_sec"]
    run_end = finals[-1]["end_sec"]
    run_dur = run_end - run_start

    verdict = "OK" if not notes else "ATTENTION"
    print("=" * 60)
    print(f"TIMING  [{verdict}]")
    print("=" * 60)
    print(f"  finals    : {len(finals)}")
    print(f"  run span  : {run_dur:.1f}s  ({run_start:.1f}s -> {run_end:.1f}s)")
    print(f"  gap (max) : {max_gap:.2f}s")
    print(f"  gap (mean): {mean_gap:.2f}s")
    for n in notes:
        print(f"  NOTE: {n}")
    print()


# ── section 4: transcript sample ─────────────────────────────────────────────

def transcript_sample(events, head=200, tail=200):
    finals = sorted(
        [e for e in events if e.get("is_final")],
        key=lambda e: e.get("seq", 0),
    )
    full = " ".join(e.get("text", "") for e in finals).strip()
    if not full:
        print("=" * 60)
        print("TRANSCRIPT  [no final events]")
        print("=" * 60)
        print()
        return

    print("=" * 60)
    print(f"TRANSCRIPT  [{len(full)} chars total from {len(finals)} finals]")
    print("=" * 60)
    if len(full) <= head + tail:
        print(f"  {full}")
    else:
        print(f"  ...HEAD: {full[:head]}")
        print(f"  ...TAIL: {full[-tail:]}")
    print()


# ── main ─────────────────────────────────────────────────────────────────────

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

    # Section 1: audio
    if wav_path.exists():
        analyze_wav(wav_path)
    else:
        print(f"[AUDIO section skipped: {wav_path} not found]\n")

    # Load events
    events = []
    if jsonl_path.exists():
        with open(jsonl_path, encoding="utf-8") as f:
            for lineno, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    events.append(json.loads(line))
                except json.JSONDecodeError as e:
                    print(f"  [WARN] line {lineno}: JSON parse error: {e}", file=sys.stderr)
    else:
        print(f"[EVENTS/TIMING/TRANSCRIPT sections skipped: {jsonl_path} not found]\n")

    if events:
        analyze_events(events)
        analyze_timing(events)
        transcript_sample(events)


if __name__ == "__main__":
    main()
