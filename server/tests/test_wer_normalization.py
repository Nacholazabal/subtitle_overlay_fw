import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from server.evaluation.dataset_manifest import normalize_spanish_text
from server.evaluation.wer_normalization import (
    normalize_for_wer,
    normalized_error_rate,
)


class WerNormalizationTests(unittest.TestCase):
    def test_legacy_profile_is_exactly_the_historical_normalizer(self):
        samples = [
            "  ¿Qué TAL, número 22? ",
            "¡Hola!  Dos cafés, por favor.",
            "",
            "NFKC: １２３",
        ]
        for text in samples:
            self.assertEqual(
                normalize_spanish_text(text), normalize_for_wer(text, profile="legacy")
            )

    def test_numeric_profile_aligns_spelled_and_written_cardinals(self):
        for words, digits in (
            ("dieciséis", "16"),
            ("veintidós", "22"),
            ("treinta y dos", "32"),
            ("ciento veinticinco", "125"),
            ("ciento cincuenta", "150"),
            ("ciento cincuenta y dos", "152"),
            ("setecientos treinta y cuatro", "734"),
            ("dos mil treinta y cinco", "2035"),
        ):
            with self.subTest(words=words):
                self.assertEqual("son " + digits + " personas", normalize_for_wer(
                    "son " + words + " personas", profile="numeric_es"
                ))
                self.assertEqual(0.0, normalized_error_rate(
                    "son " + words + " personas", "son " + digits + " personas",
                    profile="numeric_es",
                ).rate)

    def test_spoken_decimal_and_character_scoring(self):
        self.assertEqual("temperatura 2.5 grados", normalize_for_wer(
            "temperatura dos coma cinco grados", profile="numeric_es"
        ))
        self.assertEqual("temperatura 2.5 grados", normalize_for_wer(
            "temperatura 2,5 grados", profile="numeric_es"
        ))
        self.assertEqual("temperatura 2.5 grados", normalize_for_wer(
            "temperatura 2.5 grados", profile="numeric_es"
        ))
        self.assertEqual("población 1 000 personas", normalize_for_wer(
            "población 1.000 personas", profile="numeric_es"
        ))
        result = normalized_error_rate("hola", "ola", unit="character")
        self.assertEqual(1, result.edits)
        self.assertEqual(4, result.reference_units)
        self.assertEqual(0.25, result.rate)

    def test_unknown_profile_and_unit_are_rejected(self):
        with self.assertRaises(ValueError):
            normalize_for_wer("hola", profile="mystery")
        with self.assertRaises(ValueError):
            normalized_error_rate("hola", "hola", unit="phoneme")


if __name__ == "__main__":
    unittest.main()
