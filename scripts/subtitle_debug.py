#!/usr/bin/env python3
"""Replay STT events and reconstruct what the subtitle overlay shows.

This mirrors the firmware so a run can be inspected without the board:
- subtitle_text_sanitize.c  -> UTF-8 folded to approximate ASCII
- SubtitleAO.c              -> rolling transcript (last N words; finals commit,
                               partials show history + in-progress text)
- subtitle_text_renderer.c  -> newest text wrapped into the 5-line 1024x256 mask

Usage:
  python scripts/subtitle_debug.py [events.jsonl]      # replay a recorded run
  python scripts/subtitle_debug.py --follow [path]     # live view as it runs
  python scripts/subtitle_debug.py --all [path]        # show partials too
"""

import argparse
import json
import os
import sys
import time

# Must match the firmware constants.
HISTORY_WORDS = 48           # SUBTITLE_AO_HISTORY_WORDS
MASK_WIDTH = 1024            # SUBTITLE_BRAM_MASK_WIDTH
GLYPH_SCALE = 4              # GLYPH_SCALE
GLYPH_ADVANCE = 7 * GLYPH_SCALE
RENDER_TEXT_X = 24           # RENDER_TEXT_X
GLYPH_RENDERED_WIDTH = 5 * GLYPH_SCALE
GLYPHS_PER_LINE = (
    ((MASK_WIDTH - RENDER_TEXT_X - GLYPH_RENDERED_WIDTH) // GLYPH_ADVANCE) + 1
)
MAX_LINES = 5                # RENDER_MAX_LINES

# UTF-8 -> ASCII folding, matching subtitle_text_sanitize.c.
_SANITIZE_MAP = {
    "á": "a", "é": "e", "í": "i", "ó": "o", "ú": "u", "ü": "u", "ñ": "n",
    "Á": "A", "É": "E", "Í": "I", "Ó": "O", "Ú": "U", "Ü": "U", "Ñ": "N",
    "¿": " ", "¡": " ",
    "“": "'", "”": "'", "‘": "'", "’": "'", '"': "'",
    "–": "-", "—": "-", "…": ".",
}


def sanitize(text):
    out = []
    for ch in text:
        code = ord(ch)
        if 0x20 <= code <= 0x7E:
            out.append("'" if ch == '"' else ch)
        elif code < 0x20:
            out.append(" ")
        else:
            out.append(_SANITIZE_MAP.get(ch, " "))
    return "".join(out)


def last_n_words(text, n):
    words = text.split()
    if len(words) <= n:
        return text.strip()
    return " ".join(words[-n:])


def wrap_tail(text, width, max_lines):
    """Greedy word-wrap, then keep the last max_lines lines (the newest text)."""
    lines = []
    current = ""
    for word in text.split():
        candidate = word if not current else current + " " + word
        if len(candidate) <= width:
            current = candidate
        else:
            if current:
                lines.append(current)
            # A single word longer than the line is hard-cut.
            while len(word) > width:
                lines.append(word[:width])
                word = word[width:]
            current = word
    if current:
        lines.append(current)
    return lines[-max_lines:] if lines else [""]


class SubtitleModel:
    def __init__(self):
        self.history = ""

    def apply(self, text, is_final):
        combined = f"{self.history} {text}".strip() if self.history else text
        if is_final:
            self.history = last_n_words(combined, HISTORY_WORDS)
            display = self.history
        else:
            display = last_n_words(combined, HISTORY_WORDS)
        return wrap_tail(sanitize(display), GLYPHS_PER_LINE, MAX_LINES)


def render_box(lines):
    width = GLYPHS_PER_LINE
    border = "+" + "-" * (width + 2) + "+"
    body = "\n".join(f"| {ln.ljust(width)} |" for ln in lines)
    return f"{border}\n{body}\n{border}"


def iter_events(path, follow):
    if path == "-":
        for line in sys.stdin:
            yield line
        return

    with open(path, "r", encoding="utf-8") as handle:
        while True:
            line = handle.readline()
            if line:
                yield line
            elif follow:
                time.sleep(0.2)
            else:
                return


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "path",
        nargs="?",
        default="logs/stt_events.jsonl",
        help="JSONL of STT events, or '-' for stdin (default: logs/stt_events.jsonl)",
    )
    parser.add_argument("--follow", action="store_true", help="keep reading as the run produces events")
    parser.add_argument("--all", action="store_true", help="show partial events too, not only finals")
    args = parser.parse_args()

    if args.path != "-" and not os.path.exists(args.path):
        parser.error(f"no event log at {args.path} (run the sender with --jsonl first)")

    model = SubtitleModel()
    for raw in iter_events(args.path, args.follow):
        raw = raw.strip()
        if not raw:
            continue
        try:
            event = json.loads(raw)
        except json.JSONDecodeError:
            continue

        is_final = bool(event.get("is_final"))
        text = event.get("text", "")
        lines = model.apply(text, is_final)
        if not args.all and not is_final:
            continue

        kind = "FINAL  " if is_final else "partial"
        print(f"[{kind} seq={event.get('seq')}] heard={text!r}")
        print(render_box(lines))
        print()

    print("=== full transcript (committed finals) ===")
    print(model.history)


if __name__ == "__main__":
    main()
