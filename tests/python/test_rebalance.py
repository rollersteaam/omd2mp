import os, sys, tempfile, unittest, json
ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "mods", "rebalance"))
import build_rebalance as rb

WAVE = """<!-- (c) header comment must survive -->
<Wave Break="5">
  <Group Count="1" EntitySpacing="0.5">
    <Entity Count="8">MobOrcLight</Entity>
    <Entity Count="2" Coop="">MobKoboldRunner</Entity>
    <Entity Count="3" startaction="AttachShield" ActionPercent="1.0" Coop="">MobOrcMedium</Entity>
    <Entity Coop="" Count="5">MobTroll</Entity>
  </Group>
  <OpenDoor Coop="">235</OpenDoor>
</Wave>
"""


class ScaledCount(unittest.TestCase):
    def test_two_players_is_vanilla(self):
        for c in range(0, 50):
            self.assertEqual(rb.scaled_count(c, 2, 1.0), c)

    def test_linear_in_extra_players(self):
        self.assertEqual(rb.scaled_count(2, 4, 1.0), 6)
        self.assertEqual(rb.scaled_count(2, 8, 1.0), 14)

    def test_scale_rounds_half_up_and_never_negative(self):
        self.assertEqual(rb.scaled_count(3, 2, 0.5), 2)   # 1.5 -> 2
        self.assertEqual(rb.scaled_count(1, 2, 0.0), 0)


class RebalanceText(unittest.TestCase):
    def test_identity_at_two_players(self):
        out, st = rb.rebalance_text(WAVE, 2)
        self.assertEqual(out, WAVE)
        self.assertEqual(st["coop_entities"], 3)

    def test_four_players(self):
        out, st = rb.rebalance_text(WAVE, 4)
        self.assertIn('<Entity Count="8">MobOrcLight', out)                 # solo mobs untouched
        self.assertIn('<Entity Count="6" Coop="">MobKoboldRunner', out)
        self.assertIn('<Entity Count="9" startaction="AttachShield" ActionPercent="1.0" Coop="">', out)
        self.assertIn('<Entity Coop="" Count="15">MobTroll', out)          # attribute order irrelevant
        self.assertIn('<OpenDoor Coop="">235</OpenDoor>', out)             # doors untouched
        self.assertIn("(c) header comment must survive", out)
        self.assertEqual((st["mobs_before"], st["mobs_after"]), (10, 30))

    def test_only_count_changes(self):
        out, _ = rb.rebalance_text(WAVE, 8)
        strip = lambda s: [l for l in s.splitlines() if "Coop" not in l or "Entity" not in l]
        self.assertEqual(strip(out), strip(WAVE))


class BuildTree(unittest.TestCase):
    def test_build_writes_changed_files_only_and_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "src", "onslaughts")
            os.makedirs(src)
            open(os.path.join(src, "A.onslaught"), "w").write(WAVE)
            open(os.path.join(src, "B.Onslaught"), "w").write(WAVE)        # Robot ships both cases
            open(os.path.join(src, "C.onslaught"), "w").write('<Entity Count="1">MobOrcLight</Entity>')
            open(os.path.join(src, "X.timing"), "w").write(WAVE)
            out = os.path.join(d, "out")
            m = rb.build(os.path.join(d, "src"), out, 4, 1.0)
            files = sorted(os.listdir(os.path.join(out, "mod", "game", "onslaughts")))
            self.assertEqual(files, ["A.onslaught", "B.Onslaught"])
            self.assertEqual(m["totals"]["coop_entities"], 6)
            self.assertEqual(json.load(open(os.path.join(out, "rebalance_manifest.json")))["players"], 4)


if __name__ == "__main__":
    unittest.main()
