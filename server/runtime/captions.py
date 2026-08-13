"""Shared audio conversion and firmware-safe live caption formatting."""

TARGET_RATE = 16000
VISIBLE_TEXT_MAX_CHARS = 120
DISPLAY_LINE_MAX_CHARS = 55
SENTENCE_ENDINGS = (".", "?", "!", "…")


def pcm_s16le_to_float32(pcm_bytes: bytes):
    """Decode little-endian S16 mono PCM into float32 samples."""
    import numpy as np

    if len(pcm_bytes) < 2:
        return np.zeros(0, dtype="float32")
    samples = np.frombuffer(pcm_bytes[: len(pcm_bytes) & ~1], dtype="<i2")
    return samples.astype("float32") / 32768.0


def resample_to_16k(mono_float32, source_rate: int):
    """Linearly resample mono float32 audio to Nemotron's 16 kHz input."""
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


def bounded_tail(text: str, max_chars: int = VISIBLE_TEXT_MAX_CHARS) -> str:
    """Return the longest whole-word suffix fitting the firmware message."""
    if len(text) <= max_chars:
        return text
    kept: list[str] = []
    total = 0
    for word in reversed(text.split()):
        extra = len(word) + (1 if kept else 0)
        if total + extra > max_chars:
            break
        kept.insert(0, word)
        total += extra
    return " ".join(kept) if kept else text[-max_chars:]

