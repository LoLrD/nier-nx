import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "lang"))

import textfiles  # noqa: E402

TKD = "1000\t\t0\t1\t0\tEID_0050_a\t\t\t\t\t-1\t0\t0\t0\t25\tYonah!?\r\n\t\t0\t1\t0\t\t\t\t\t\t-1\t0\t0\t0\t26\tLine one\nline two\r\n".encode()
INW = "2000\t\t1\t0\tIN_EID\t\t\t\t\t\t\t0\tControls\tPress <ICON=2> to attack.\r\n".encode()
TXD = "1100000\tセーブしますか？\tSave?\tSauvegarder ?\tSpeichern?\r\n".encode()


class TextFilesTest(unittest.TestCase):
    def test_texts_are_keyed_by_record_and_column(self) -> None:
        self.assertEqual(textfiles.texts(TKD, "TEXT/TALK_TEXT/TALK_A.EN.X360.TKD"),
                         {"0:15": "Yonah!?", "1:15": "Line one\nline two"})
        self.assertEqual(textfiles.texts(INW, "TEXT/INFO_TEXT/INFOWINDOW.EN.INW"),
                         {"0:12": "Controls", "0:13": "Press <ICON=2> to attack."})
        self.assertEqual(textfiles.texts(TXD, "TEXT/NIER_TEXT.TXD"), {"0:2": "Save?"})

    def test_nothing_translated_gives_the_same_bytes(self) -> None:
        for data, path in [(TKD, "TEXT/TALK_TEXT/TALK_A.EN.X360.TKD"), (INW, "TEXT/INFO_TEXT/INFOWINDOW.EN.INW"),
                           (TXD, "TEXT/NIER_TEXT.TXD")]:
            self.assertEqual(textfiles.apply(data, path, {}), data)

    def test_only_translated_keys_change(self) -> None:
        out = textfiles.apply(TKD, "TEXT/TALK_TEXT/TALK_A.EN.X360.TKD", {"1:15": "Строка один\nстрока два"})
        self.assertEqual(textfiles.texts(out, "TEXT/TALK_TEXT/TALK_A.EN.X360.TKD"),
                         {"0:15": "Yonah!?", "1:15": "Строка один\nстрока два"})
        self.assertTrue(out.endswith(b"\r\n"))
        self.assertEqual(out.split(b"\r\n")[1].split(b"\t")[:15], TKD.split(b"\r\n")[1].split(b"\t")[:15])

    def test_an_unknown_key_is_refused(self) -> None:
        with self.assertRaises(KeyError):
            textfiles.apply(TKD, "TEXT/TALK_TEXT/TALK_A.EN.X360.TKD", {"5:15": "Нет"})

    def test_lost_tags_and_tabs_are_reported(self) -> None:
        self.assertEqual(textfiles.problems("Press <ICON=2> now", "Нажмите <ICON=2>"), [])
        self.assertEqual(textfiles.problems("Press <ICON=2> now", "Нажмите сейчас"), ["tags differ"])
        self.assertEqual(textfiles.problems("A", "Б\tВ"), ["tab or CR in text"])

    def test_a_lost_percent_sign_is_reported(self) -> None:
        self.assertEqual(textfiles.problems("Rate: 50%%", "Шанс: 50%"), ["tags differ"])

    def test_structure_tags_keep_their_order_but_placeholders_may_move(self) -> None:
        self.assertEqual(textfiles.problems("<LIST=1>Yes</LIST><LIST=2>No</LIST>",
                                            "<LIST=2>Нет</LIST><LIST=1>Да</LIST>"), ["tag order differs"])
        self.assertEqual(textfiles.problems("Give <ITEM> to <NIER>?<PAGE>Ok", "<NIER> получает <ITEM>?<PAGE>Да"), [])

    def test_talker_names_use_the_english_column(self) -> None:
        data = "5\tヨナ\tYonah\tYonah\tYonah\r\n".encode()
        self.assertEqual(textfiles.texts(data, "TEXT/TALK_TEXT/TALKER_NAME.X360.TND"), {"0:2": "Yonah"})

    def test_switch_english_names_the_plus_and_minus_buttons(self) -> None:
        self.assertEqual(textfiles.switch_english("Press START to Skip"), "Press <ICON=13> to Skip")
        self.assertEqual(textfiles.switch_english("Press the BACK button"), "Press the <ICON=12> button")
        self.assertEqual(textfiles.switch_english("RESTART the BACKUP"), "RESTART the BACKUP")

    def test_table_escaping_round_trips(self) -> None:
        rows = {"0:15": "Строка\nдва \\ слэш", "1:15": "Да"}
        self.assertEqual(textfiles.parse_table(textfiles.format_table(rows)), rows)


if __name__ == "__main__":
    unittest.main()
