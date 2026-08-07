#!/usr/bin/env python3
"""Deterministic MediaSpeech ES dataset preparation for the final STT evaluation.

This module deliberately contains no model code.  It verifies the immutable
OpenSLR archive, extracts it safely into Colab's local disk and builds a stable
manifest of the one-to-one FLAC/TXT pairs.  Paths stored in the manifest are
relative so a checkpoint written to Drive remains usable after a Colab restart.
"""

from __future__ import annotations

import hashlib
import json
import os
import tarfile
import unicodedata
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


DATASET_ID = "mediaspeech_es"
DATASET_VERSION = "1.1"
ARCHIVE_NAME = "ES.tgz"
ARCHIVE_SHA256 = "07917baf12467f1467dd525d1f4747a807ba938e36156382ae5229a89d76bf52"
EXPECTED_CLIPS = 2507


class DatasetValidationError(RuntimeError):
    """The dataset is incomplete, ambiguous or different from the fixed corpus."""


@dataclass(frozen=True)
class DatasetClip:
    """One human-labelled MediaSpeech utterance."""

    clip_id: str
    audio_relpath: str
    reference_relpath: str
    audio_sha256: str
    reference_sha256: str
    reference_raw: str
    reference_normalized: str
    reference_words: int

    def as_dict(self) -> dict:
        return asdict(self)

    def audio_path(self, dataset_root: Path) -> Path:
        return Path(dataset_root) / self.audio_relpath

    def reference_path(self, dataset_root: Path) -> Path:
        return Path(dataset_root) / self.reference_relpath


def sha256_file(path: Path, *, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        while chunk := handle.read(chunk_size):
            digest.update(chunk)
    return digest.hexdigest()


def verify_archive(path: Path, expected_sha256: str = ARCHIVE_SHA256) -> dict:
    """Verify the exact MediaSpeech archive before any extraction."""
    path = Path(path).expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError(f"MediaSpeech archive not found: {path}")
    actual = sha256_file(path)
    if actual != expected_sha256:
        raise DatasetValidationError(
            f"unexpected {path.name} SHA-256: {actual}; expected {expected_sha256}"
        )
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": actual}


def _safe_member_destination(destination: Path, member_name: str) -> Path:
    target = (destination / member_name).resolve()
    try:
        target.relative_to(destination.resolve())
    except ValueError as exc:
        raise DatasetValidationError(f"unsafe tar member path: {member_name!r}") from exc
    return target


def safe_extract_archive(archive: Path, destination: Path) -> Path:
    """Extract regular files/directories only, rejecting links and path traversal."""
    archive = Path(archive).resolve()
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as bundle:
        members = bundle.getmembers()
        for member in members:
            _safe_member_destination(destination, member.name)
            if not (member.isfile() or member.isdir()):
                raise DatasetValidationError(
                    f"unsupported tar member type for {member.name!r}; links are not allowed"
                )
        bundle.extractall(destination, members=members)
    return destination


def normalize_spanish_text(text: str) -> str:
    """Case/punctuation-insensitive normalization used consistently for WER/CER."""
    normalized = unicodedata.normalize("NFKC", text).casefold()
    return " ".join(
        "".join(character if character.isalnum() else " " for character in normalized).split()
    )


def _unique_by_stem(paths: Iterable[Path], suffix: str) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for path in sorted(paths):
        stem = path.stem
        if stem in result:
            raise DatasetValidationError(
                f"duplicate {suffix} clip id {stem!r}: {result[stem]} and {path}"
            )
        result[stem] = path
    return result


def discover_dataset_root(extracted_root: Path) -> Path:
    """Find the unique directory below ``extracted_root`` containing all pairs."""
    extracted_root = Path(extracted_root).resolve()
    if not extracted_root.is_dir():
        raise FileNotFoundError(f"dataset extraction directory not found: {extracted_root}")
    flacs = list(extracted_root.rglob("*.flac"))
    if not flacs:
        raise DatasetValidationError(f"no FLAC files below {extracted_root}")
    common = Path(os.path.commonpath([str(path.parent) for path in flacs]))
    return common.resolve()


def build_mediaspeech_manifest(
    dataset_root: Path,
    *,
    expected_clips: int = EXPECTED_CLIPS,
) -> list[DatasetClip]:
    """Validate exact FLAC/TXT pairing and hash every labelled utterance."""
    dataset_root = Path(dataset_root).resolve()
    audio_by_id = _unique_by_stem(dataset_root.rglob("*.flac"), "audio")
    text_by_id = _unique_by_stem(dataset_root.rglob("*.txt"), "reference")
    missing_text = sorted(set(audio_by_id) - set(text_by_id))
    missing_audio = sorted(set(text_by_id) - set(audio_by_id))
    if missing_text or missing_audio:
        raise DatasetValidationError(
            "MediaSpeech FLAC/TXT pairing mismatch: "
            f"missing_txt={missing_text[:5]} missing_flac={missing_audio[:5]}"
        )
    if len(audio_by_id) != expected_clips:
        raise DatasetValidationError(
            f"expected {expected_clips} MediaSpeech clips, found {len(audio_by_id)}"
        )

    clips: list[DatasetClip] = []
    for clip_id in sorted(audio_by_id):
        audio = audio_by_id[clip_id]
        reference = text_by_id[clip_id]
        raw = reference.read_text(encoding="utf-8").strip()
        normalized = normalize_spanish_text(raw)
        if not normalized:
            raise DatasetValidationError(f"empty normalized reference: {reference}")
        clips.append(
            DatasetClip(
                clip_id=clip_id,
                audio_relpath=audio.relative_to(dataset_root).as_posix(),
                reference_relpath=reference.relative_to(dataset_root).as_posix(),
                audio_sha256=sha256_file(audio),
                reference_sha256=sha256_file(reference),
                reference_raw=raw,
                reference_normalized=normalized,
                reference_words=len(normalized.split()),
            )
        )
    return clips


def manifest_fingerprint(clips: Iterable[DatasetClip]) -> str:
    """Content identity independent of extraction path and JSON formatting."""
    digest = hashlib.sha256()
    for clip in clips:
        digest.update(clip.clip_id.encode("utf-8"))
        digest.update(b"\0")
        digest.update(clip.audio_sha256.encode("ascii"))
        digest.update(b"\0")
        digest.update(clip.reference_sha256.encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def write_manifest(path: Path, clips: Iterable[DatasetClip]) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        for clip in clips:
            handle.write(json.dumps(clip.as_dict(), ensure_ascii=False, sort_keys=True) + "\n")
        handle.flush()
        os.fsync(handle.fileno())
    temporary.replace(path)


def read_manifest(path: Path) -> list[DatasetClip]:
    clips: list[DatasetClip] = []
    with Path(path).open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            if not line.strip():
                continue
            try:
                clips.append(DatasetClip(**json.loads(line)))
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                raise DatasetValidationError(
                    f"invalid manifest record at {path}:{line_number}"
                ) from exc
    return clips
