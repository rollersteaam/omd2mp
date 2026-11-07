"""Tests for tools/install.py's ini repair (pure text, no game files)."""
import configparser, os, sys, unittest
ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import install

TEMPLATE = open(os.path.join(ROOT, "dist", "omd2mp.ini")).read()
OLD = "[omd2mp]\nenabled=1\nplayers=3\ngroups=lobby,mesh\n"


def parse(text):
    c = configparser.ConfigParser(interpolation=None)
    c.read_string(text)
    return c


class MergeIni(unittest.TestCase):
    def test_one_line_section_is_split_and_kept(self):
        text, notes = install.merge_ini(OLD + "\n[dev] enabled=1", TEMPLATE)
        c = parse(text)
        self.assertEqual(c["dev"]["enabled"], "1")              # user's value wins
        self.assertEqual(c["omd2mp"]["players"], "3")
        self.assertNotIn("pipe", c["dev"])                      # section existed, not re-added
        self.assertTrue(any("split" in n for n in notes))

    def test_missing_dev_section_added_with_defaults(self):
        text, notes = install.merge_ini(OLD, TEMPLATE)
        c = parse(text)
        self.assertEqual(c["dev"]["enabled"], "0")
        self.assertEqual(c["dev"]["pipe"], "omd2mp-dev")
        self.assertEqual(c["omd2mp"]["groups"], "lobby,mesh")
        self.assertEqual(c["mpdebug"]["enabled"], "0")            # mocks stay off on upgrade
        self.assertEqual(notes, ["added missing [dev] section with defaults",
                                 "added missing [mpdebug] section with defaults",
                                 "added missing [nettrace] section with defaults"])
        self.assertEqual(c["nettrace"]["enabled"], "1")

    def test_good_file_untouched(self):
        text, notes = install.merge_ini(TEMPLATE, TEMPLATE)
        self.assertEqual(notes, [])

    def test_comment_after_header_is_fine(self):
        _, notes = install.merge_ini(OLD + "[dev] ; console\nenabled=1\n", TEMPLATE)
        self.assertFalse(any("[dev]" in n or "split" in n for n in notes), notes)


if __name__ == "__main__":
    unittest.main()
