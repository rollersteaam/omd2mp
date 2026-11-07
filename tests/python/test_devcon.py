"""Tests for tools/devcon.py against a fake console that speaks devcon.c's protocol."""
import csv, io, json, os, shlex, sys, tempfile, unittest
ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import devcon


class FakeGame:
    """Answers console lines the way loader/src/devcon.c does (reply shapes copied from it)."""

    def __init__(self, players=4, wave=1):
        self.players, self.wave, self.ticks = players, wave, 0
        self.next_wave_time, self.won, self.paused = 5.0, False, False
        self.since_rx = [0.0] * 8
        self.lines, self.timescale, self.ready_presses = [], 1.0, 0
        self.dead = False

    def value(self, v):
        if v is None:
            return {"ok": True, "returned": False, "result": {"type": "undefined"}}
        t = "bool" if isinstance(v, bool) else "number" if isinstance(v, (int, float)) else "string"
        return {"ok": True, "returned": True, "result": {"type": t, "value": v}}

    def handle(self, line):
        self.lines.append(line)
        self.ticks += 1
        t = shlex.split(line)
        c = t[0]
        if c == "hello":
            return {"ok": True, "name": "omd2mp-dev", "version": 1, "ticks": self.ticks, "ui": True,
                    "players": self.players, "slots_patched": True, "allow_write": False}
        if c == "ping":
            return {"ok": True, "ticks": self.ticks}
        if c == "log":
            return {"ok": True}
        if c == "timescale":
            self.timescale = float(t[1])
            return {"ok": True, "timescale": self.timescale}
        if c == "session":
            return {"ok": True, "session": {"addr": "0x1234", "local_slot": 0, "player_count": self.players,
                                            "slot_base": "0x36068", "slots": []}}
        if c == "read":
            return {"ok": True, "addr": t[1], "hex": "00" * int(t[2])}
        if c == "call":
            name, args = t[1], t[2:]
            g = {"Net_IsMultiplayer": True, "Net_IsHost": True, "Net_MyIndex": 0,
                 "Net_PlayerCount": self.players, "Level_GetCurrentWaveNumber": self.wave,
                 "Level_GetCurrentWaveTotal": 0, "Level_GetNextWaveTime": self.next_wave_time,
                 "Level_GetCurrentIsEndless": True, "Level_GetCurrentWon": self.won,
                 "Game_IsPaused": self.paused, "Game_GetSecondsInGame": self.ticks * 1.0}
            if name in g:
                return self.value(g[name])
            if name == "Player_GetCash":
                return self.value(1000 + int(args[0]))
            if name == "Net_GetPingMS":
                return self.value(30)
            if name == "Net_GetTimeSinceLastReceived":
                return self.value(self.since_rx[int(args[0])])
            if name == "Level_ReadyUp":
                assert args == ["true"], args
                self.ready_presses += 1
                return self.value(None)
            return {"ok": False, "error": "unknown binding (names are case-sensitive; see research/bindings.tsv)"}
        return {"ok": False, "error": "unknown command (hello ping call session timescale read write log)"}


class FakeTransport:
    def __init__(self, game):
        self.game, self.pending = game, []

    def send(self, line):
        if self.game.dead:
            raise devcon.ConnectionLost("pipe closed")
        assert "\n" not in line
        self.pending.append(json.dumps(self.game.handle(line)))

    def recv(self):
        return self.pending.pop(0)


def client(game):
    return devcon.Client(FakeTransport(game))


class Quote(unittest.TestCase):
    def test_values(self):
        self.assertEqual(devcon.quote(True), "true")
        self.assertEqual(devcon.quote(False), "false")
        self.assertEqual(devcon.quote(None), "null")
        self.assertEqual(devcon.quote(3), "3")
        self.assertEqual(devcon.quote(-2.5), "-2.5")
        self.assertEqual(devcon.quote("a b"), '"a b"')
        self.assertEqual(devcon.quote("3"), '"3"')                 # strings stay strings
        self.assertEqual(devcon.quote('x"y\\z\nw'), '"x\\"y\\\\z\\nw"')

    def test_roundtrips_through_console_tokeniser(self):
        # shlex (posix) handles \" and \\ like dev_split does; \n is ours alone.
        for s in ["plain", "with space", 'q"uote', "back\\slash"]:
            self.assertEqual(shlex.split("log " + devcon.quote(s))[1], s)

    def test_rejects_nan(self):
        with self.assertRaises(ValueError):
            devcon.quote(float("nan"))


class ClientCalls(unittest.TestCase):
    def test_call_returns_value(self):
        g = FakeGame()
        c = client(g)
        self.assertEqual(c.call("Player_GetCash", 2), 1002)
        self.assertEqual(g.lines[-1], "call Player_GetCash 2")
        self.assertIsNone(c.call("Level_ReadyUp", True))

    def test_error_raises(self):
        c = client(FakeGame())
        with self.assertRaises(devcon.DevError) as e:
            c.call("Nope")
        self.assertIn("unknown binding", str(e.exception))

    def test_raw_does_not_raise(self):
        self.assertFalse(client(FakeGame()).raw("bogus")["ok"])

    def test_read_and_timescale(self):
        g = FakeGame()
        c = client(g)
        self.assertEqual(c.read(0xB9D108, 4), b"\0\0\0\0")
        self.assertEqual(g.lines[-1], "read 0xb9d108 4")
        self.assertEqual(c.timescale(4), 4.0)
        self.assertEqual(g.timescale, 4.0)


class State(unittest.TestCase):
    def test_snapshot(self):
        g = FakeGame(players=3, wave=7)
        st = client(g).state()
        self.assertEqual(st["wave"], 7)
        self.assertEqual(st["player_count"], 3)
        self.assertEqual([p["cash"] for p in st["players"]], [1000, 1001, 1002])
        self.assertEqual(st["session"]["slot_base"], "0x36068")
        self.assertNotIn("errors", st)

    def test_missing_binding_is_recorded_not_raised(self):
        g = FakeGame()
        orig = g.handle
        g.handle = lambda line: ({"ok": False, "error": "boom"} if "Game_IsPaused" in line else orig(line))
        st = client(g).state()
        self.assertIsNone(st["paused"])
        self.assertEqual(st["errors"], {"Game_IsPaused": "boom"})

    def test_csv_row(self):
        st = client(FakeGame(players=2)).state()
        row = devcon.flat(st)
        self.assertEqual(set(row), set(devcon.csv_fields()))
        self.assertEqual(row["p1_cash"], 1001)
        self.assertIsNone(row["p5_cash"])


class SoakRun(unittest.TestCase):
    def make(self, g, **kw):
        self.t = [0.0]
        def sleep(s):
            self.t[0] += s
            g.wave += 1                  # one wave per sample keeps the tests short
        out = io.StringIO()
        s = devcon.Soak(client(g), clock=lambda: self.t[0], sleep=sleep, echo=lambda *_: None, out=out, **kw)
        return s, out

    def test_reaches_wave_and_restores_timescale(self):
        g = FakeGame(wave=1)
        s, out = self.make(g, until_wave=5, timescale=8, ready_up=True)
        self.assertEqual(s.run(), "reached wave 5")
        self.assertEqual(g.timescale, 1.0)
        self.assertEqual(g.ready_presses, 4)
        kinds = [json.loads(l)["kind"] for l in out.getvalue().splitlines()]
        self.assertEqual(kinds.count("wave"), 4)
        self.assertEqual(kinds[-1], "end")
        self.assertTrue(any(l.startswith("log ") and "soak start" in l for l in g.lines))

    def test_no_ready_up_when_no_countdown(self):
        g = FakeGame(wave=1)
        g.next_wave_time = 0
        s, _ = self.make(g, until_wave=3, ready_up=True)
        s.run()
        self.assertEqual(g.ready_presses, 0)

    def test_flags_player_drop_and_silent_peer(self):
        g = FakeGame(players=4, wave=1)
        s, _ = self.make(g, until_wave=4)
        orig_sleep = s.sleep
        def sleep(x):
            orig_sleep(x)
            if g.wave == 3:
                g.since_rx[2] = 15.0     # peer 2 goes quiet, then drops
            if g.wave == 4:
                g.players = 3
        s.sleep = sleep
        s.run()
        kinds = [e["kind"] for e in s.events]
        self.assertIn("silent_peer", kinds)
        d = [e for e in s.events if e["kind"] == "disconnect"]
        self.assertEqual(len(d), 1)
        self.assertEqual((d[0]["was"], d[0]["now"]), (4, 3))
        self.assertEqual(s.events[-1]["disconnects"], 1)

    def test_own_slot_silence_ignored(self):
        g = FakeGame(players=2)
        g.since_rx[0] = 99.0             # my_index is 0
        s, _ = self.make(g, until_wave=2)
        s.run()
        self.assertNotIn("silent_peer", [e["kind"] for e in s.events])

    def test_connection_lost(self):
        g = FakeGame(wave=1)
        s, _ = self.make(g, until_wave=100)
        orig_sleep = s.sleep
        def sleep(x):
            orig_sleep(x)
            if g.wave == 3:
                g.dead = True
        s.sleep = sleep
        self.assertEqual(s.run(), "connection lost")
        self.assertEqual(s.events[-1]["reason"], "connection lost")

    def test_won_and_time_limit(self):
        g = FakeGame(wave=1)
        g.won = True
        s, _ = self.make(g, until_wave=100)
        self.assertEqual(s.run(), "won")
        g = FakeGame(wave=1)
        s, _ = self.make(g, max_minutes=0.05, interval=1.0)
        self.assertEqual(s.run(), "time limit")


class Cli(unittest.TestCase):
    def test_raw_and_state(self):
        g = FakeGame()
        buf = io.StringIO()
        old, sys.stdout = sys.stdout, buf
        try:
            self.assertEqual(devcon.main(["hello"], transport=FakeTransport(g)), 0)
            self.assertEqual(devcon.main(["state"], transport=FakeTransport(g)), 0)
            self.assertEqual(devcon.main(["bogus"], transport=FakeTransport(g)), 1)
        finally:
            sys.stdout = old
        self.assertIn('"omd2mp-dev"', buf.getvalue())

    def test_watch_csv(self):
        g = FakeGame(players=2)
        d = tempfile.mkdtemp()
        path = os.path.join(d, "w.csv")
        old, sys.stdout = sys.stdout, io.StringIO()
        try:
            devcon.main(["watch", "--count", "1", "--csv", path], transport=FakeTransport(g))
            devcon.main(["watch", "--count", "1", "--csv", path], transport=FakeTransport(g))
        finally:
            sys.stdout = old
        with open(path, newline="") as f:
            rows = list(csv.DictReader(f))
        self.assertEqual(len(rows), 2)                # header written once
        self.assertEqual(rows[0]["p1_cash"], "1001")


if __name__ == "__main__":
    unittest.main()
