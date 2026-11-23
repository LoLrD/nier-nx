import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "lang"))

import work  # noqa: E402

TKD = "TEXT/TALK_TEXT/TALK_A.EN.X360.TKD"


def entry(key: str, ident: str, words: int) -> dict:
    return {"path": TKD, "key": key, "id": ident, "en": " ".join(["word"] * words), "ja": ""}


class WorkTest(unittest.TestCase):
    def test_chunks_end_where_a_scene_ends(self) -> None:
        entries = [entry("0:15", "a", 4), entry("1:15", "a", 4), entry("2:15", "a", 4), entry("3:15", "b", 4),
                   entry("4:15", "b", 4), entry("5:15", "c", 4)]
        chunks = work.chunk(entries, words=6)
        self.assertEqual([[e["key"] for e in c] for c in chunks], [["0:15", "1:15", "2:15"], ["3:15", "4:15"], ["5:15"]])

    def test_ids_carry_over_continuation_records(self) -> None:
        rows = [["1000", "", "0", "1", "0", "EID_0050_a_NIER030_Y"] + [""] * 10,
                [""] * 16,
                ["1001", "", "0", "1", "0", "EID_0060_b_KAINE010_Y"] + [""] * 10]
        self.assertEqual(work.record_ids(rows, TKD), ["EID_0050_a_NIER030_Y", "EID_0050_a_NIER030_Y",
                                                     "EID_0060_b_KAINE010_Y"])

    def test_english_texts_and_scene_captions_are_translated(self) -> None:
        self.assertTrue(work.text_path("TEXT/TALK_TEXT/TALK_A.EN.X360.TKD"))
        self.assertTrue(work.text_path("TEXT/NIER_TEXT.TXD"))
        self.assertTrue(work.text_path("TEXT/TALK_TEXT/TALKER_NAME.X360.TND"))
        self.assertTrue(work.text_path("EVENT/CAPTION/EID_2000_A0120.EN.CAP"))
        self.assertFalse(work.text_path("EVENT/CAPTION/EID_2000_A0120.FR.CAP"))
        self.assertFalse(work.text_path("TEXT/TALK_TEXT/TALK_A.DE.X360.TKD"))
        self.assertFalse(work.text_path("2D/TITLE.2DP"))

    def test_short_labels_of_the_menu_tables_are_names(self) -> None:
        self.assertTrue(work.is_name("TEXT/NIER_TEXT.TXD", "Beastbane"))
        self.assertFalse(work.is_name("TEXT/NIER_TEXT.TXD", "A blade forged long ago."))
        self.assertFalse(work.is_name("TEXT/NIER_TEXT.TXD", "Line one\nline two"))
        self.assertFalse(work.is_name(TKD, "Yonah"))

    def test_a_checked_chunk_reports_missing_unknown_and_broken_lines(self) -> None:
        source = [entry("0:15", "a", 1), entry("1:15", "a", 1)]
        source[1]["en"] = "Press <ICON=2> now"
        done = [(TKD, "1:15", "Нажмите сейчас"), (TKD, "9:15", "Лишнее")]
        self.assertEqual(work.issues(source, done),
                         [f"{TKD} 0:15: missing", f"{TKD} 9:15: not in the chunk", f"{TKD} 1:15: tags differ"])

    def test_repeated_long_lines_of_one_speaker_get_one_translation(self) -> None:
        line = "Perhaps we should pay a visit to the old woman."
        rows = [dict(entry("1:15", "EID_1_BOOKW010_Y", 1), en=line, ru="А"),
                dict(entry("2:15", "EID_2_BOOKW010_Y", 1), en=line, ru="Б"),
                dict(entry("3:15", "EID_3_BOOKW010_Y", 1), en=line, ru="Б"),
                dict(entry("4:15", "EID_4_KAINE010_Y", 1), en=line, ru="В"),
                dict(entry("5:15", "EID_5_BOOKW010_Y", 1), en="Short line.", ru="Г"),
                dict(entry("6:15", "EID_6_BOOKW010_Y", 1), en="Short line.", ru="Д")]
        self.assertEqual([row["ru"] for row in work.harmonize(rows)], ["Б", "Б", "Б", "В", "Г", "Д"])

    def test_merge_keeps_existing_lines_and_orders_by_record(self) -> None:
        merged = work.merge({"10:15": "Старое"}, {"2:15": "Два", "10:15": "Новое", "1:15": "Один"})
        self.assertEqual(list(merged.items()), [("1:15", "Один"), ("2:15", "Два"), ("10:15", "Старое")])


if __name__ == "__main__":
    unittest.main()
