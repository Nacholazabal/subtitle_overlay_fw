"""Alternative, explicit normalization profiles for Spanish WER/CER.

``legacy`` is byte-for-byte the existing dataset normalizer's behavior.
``numeric_es`` additionally canonicalizes common Spanish cardinal number
expressions to decimal digits. It is an evaluation aid, not NVIDIA's
undocumented scoring implementation; keep reported profiles distinct.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from server.evaluation.dataset_manifest import normalize_spanish_text


@dataclass(frozen=True)
class SequenceError:
    """Sequence-error result compatible with the evaluation harness result."""

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


_SMALL = {
    "cero": 0, "un": 1, "uno": 1, "una": 1,
    "dos": 2, "tres": 3, "cuatro": 4, "cinco": 5,
    "seis": 6, "siete": 7, "ocho": 8, "nueve": 9,
    "diez": 10, "once": 11, "doce": 12, "trece": 13,
    "catorce": 14, "quince": 15, "dieciséis": 16,
    "diecisiete": 17, "dieciocho": 18, "diecinueve": 19,
    "veinte": 20, "veintiuno": 21, "veintiún": 21, "veintiuna": 21,
    "veintidós": 22, "veintitrés": 23, "veinticuatro": 24,
    "veinticinco": 25, "veintiséis": 26, "veintisiete": 27,
    "veintiocho": 28, "veintinueve": 29,
}
_TENS = {
    "treinta": 30, "cuarenta": 40, "cincuenta": 50,
    "sesenta": 60, "setenta": 70, "ochenta": 80, "noventa": 90,
}
_HUNDREDS = {
    "cien": 100, "ciento": 100, "doscientos": 200,
    "trescientos": 300, "cuatrocientos": 400, "quinientos": 500,
    "seiscientos": 600, "setecientos": 700, "ochocientos": 800,
    "novecientos": 900,
}
_NUMBER_WORDS = set(_SMALL) | set(_TENS) | set(_HUNDREDS) | {
    "y", "mil", "coma"
}


def _parse_under_thousand(tokens: list[str]) -> int | None:
    if not tokens:
        return None
    if len(tokens) == 1:
        word = tokens[0]
        return _SMALL.get(word, _TENS.get(word, _HUNDREDS.get(word)))
    if len(tokens) == 2 and tokens[0] in _TENS and tokens[1] == "y":
        return None
    if len(tokens) == 3 and tokens[0] in _TENS and tokens[1] == "y":
        unit = _SMALL.get(tokens[2])
        if unit is not None and 1 <= unit <= 9:
            return _TENS[tokens[0]] + unit
        return None
    if len(tokens) == 2 and tokens[0] in _HUNDREDS and tokens[1] in _SMALL:
        unit = _SMALL[tokens[1]]
        if 1 <= unit <= 99:
            return _HUNDREDS[tokens[0]] + unit
    if len(tokens) == 2 and tokens[0] in _HUNDREDS and tokens[1] in _TENS:
        return _HUNDREDS[tokens[0]] + _TENS[tokens[1]]
    if (
        len(tokens) == 4
        and tokens[0] in _HUNDREDS
        and tokens[1] in _TENS
        and tokens[2] == "y"
        and tokens[3] in _SMALL
        and 1 <= _SMALL[tokens[3]] <= 9
    ):
        return _HUNDREDS[tokens[0]] + _TENS[tokens[1]] + _SMALL[tokens[3]]
    if len(tokens) == 1 and tokens[0] in _HUNDREDS:
        return _HUNDREDS[tokens[0]]
    return None


def _parse_cardinal(tokens: list[str]) -> int | None:
    """Parse conservative cardinal forms from zero through 999,999."""
    if not tokens or any(token not in _NUMBER_WORDS for token in tokens):
        return None
    if tokens.count("mil") > 1:
        return None
    if "mil" not in tokens:
        return _parse_under_thousand(tokens)
    index = tokens.index("mil")
    left = tokens[:index]
    right = tokens[index + 1:]
    thousands = 1 if not left else _parse_under_thousand(left)
    remainder = 0 if not right else _parse_under_thousand(right)
    if thousands is None or not 1 <= thousands <= 999 or remainder is None:
        return None
    return thousands * 1000 + remainder


def _numeric_normalize(text: str) -> str:
    # Preserve unambiguous numeric separators before punctuation normalization
    # removes them. Dot decimals are limited to 1-2 fractional digits so a
    # grouped thousands form such as 1.000 is not mistaken for a decimal.
    marked_text = re.sub(
        r"(?<!\w)(\d+)(?:,(\d+)|\.(\d{1,2}))(?!\w)",
        lambda match: (
            f"{match.group(1)} decimalseparator "
            f"{match.group(2) or match.group(3)}"
        ),
        text,
    )
    normalized = normalize_spanish_text(marked_text)
    tokens = normalized.split()
    result: list[str] = []
    index = 0
    while index < len(tokens):
        if (
            index + 2 < len(tokens)
            and tokens[index].isdigit()
            and tokens[index + 1] == "decimalseparator"
            and tokens[index + 2].isdigit()
        ):
            result.append(f"{tokens[index]}.{tokens[index + 2]}")
            index += 3
            continue
        # Consume the longest run that could be a cardinal; leave malformed or
        # unsupported runs untouched rather than guessing their interpretation.
        end = index
        while end < len(tokens) and tokens[end] in _NUMBER_WORDS:
            end += 1
        if end == index:
            result.append(tokens[index])
            index += 1
            continue
        run = tokens[index:end]
        # Spoken decimal support: integer cardinal + "coma" + individual
        # decimal digit words, e.g. "dos coma cinco" -> "2.5".
        if "coma" in run:
            split_at = run.index("coma")
            integer_tokens = run[:split_at]
            integer = (
                int(integer_tokens[0])
                if len(integer_tokens) == 1 and integer_tokens[0].isdigit()
                else _parse_cardinal(integer_tokens)
            )
            fraction_tokens = run[split_at + 1:]
            digits = [_SMALL.get(word) for word in fraction_tokens]
            if integer is not None and digits and all(
                digit is not None and digit < 10 for digit in digits
            ):
                result.append(f"{integer}." + "".join(str(digit) for digit in digits))
                index = end
                continue
        # Greedily use the longest parseable prefix, preserving any remaining
        # words. This avoids swallowing a following ordinary word.
        consumed = 0
        value = None
        for stop in range(len(run), 0, -1):
            value = _parse_cardinal(run[:stop])
            if value is not None:
                consumed = stop
                break
        if consumed:
            result.append(str(value))
            index += consumed
        else:
            result.append(tokens[index])
            index += 1
    return " ".join(result)


def normalize_for_wer(text: str, *, profile: str = "legacy") -> str:
    """Normalize Spanish text under an explicitly named scoring profile."""
    if profile == "legacy":
        return normalize_spanish_text(text)
    if profile == "numeric_es":
        return _numeric_normalize(text)
    raise ValueError(f"unsupported WER normalization profile: {profile}")


def _edit_distance(reference: list[str], hypothesis: list[str]) -> int:
    previous = list(range(len(hypothesis) + 1))
    for row, expected in enumerate(reference, 1):
        current = [row]
        for column, actual in enumerate(hypothesis, 1):
            current.append(min(
                current[-1] + 1,
                previous[column] + 1,
                previous[column - 1] + (expected != actual),
            ))
        previous = current
    return previous[-1]


def normalized_error_rate(
    reference: str,
    hypothesis: str,
    unit: str = "word",
    profile: str = "legacy",
) -> SequenceError:
    """Return WER (word) or CER (character, ignoring spaces) for a profile."""
    reference_normalized = normalize_for_wer(reference, profile=profile)
    hypothesis_normalized = normalize_for_wer(hypothesis, profile=profile)
    if unit == "word":
        reference_units = reference_normalized.split()
        hypothesis_units = hypothesis_normalized.split()
    elif unit == "character":
        reference_units = list(reference_normalized.replace(" ", ""))
        hypothesis_units = list(hypothesis_normalized.replace(" ", ""))
    else:
        raise ValueError(f"unsupported error-rate unit: {unit}")
    return SequenceError(
        edits=_edit_distance(reference_units, hypothesis_units),
        reference_units=len(reference_units),
        hypothesis_units=len(hypothesis_units),
    )
