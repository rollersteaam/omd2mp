"""
devcon.py - client for the omd2mp developer console (loader/src/devcon.c).

The loader opens a local named pipe when omd2mp.ini has

    [dev]
    enabled=1

and this script talks to it: one command line out, one JSON line back. Commands run on
the game's main thread, once per frame. Stdlib only.

  python tools/devcon.py                         interactive prompt (raw commands)
  python tools/devcon.py hello                   one raw command, print the reply
  python tools/devcon.py state                   wave, net and per-player state as JSON
  python tools/devcon.py watch --interval 1 --csv run.csv
  python tools/devcon.py soak --timescale 4 --until-wave 40 --ready-up --log soak.jsonl

`soak` is the high-round test: it speeds the game up, presses ready between waves, records
state every interval, and flags disconnects (player count dropping, a peer going silent,
the game dying). Run it on every machine in the session for co-op: game speed is local, so
all peers should use the same --timescale (or leave it at 1 for a faithful run).

Raw commands: hello, ping, call <Binding> [args...], session, timescale <x>,
read <addr> <len>, write <addr> <hex> (needs allow_write=1), log <text>.
See docs/CONTRIBUTING.md section 12.
"""
import argparse, csv, json, os, sys, time

DEFAULT_PIPE = "omd2mp-dev"

GLOBAL_BINDINGS = [   # binding -> state key; all take no arguments
    ("Net_IsMultiplayer", "multiplayer"),
    ("Net_IsHost", "host"),
    ("Net_MyIndex", "my_index"),
    ("Net_PlayerCount", "player_count"),
    ("Level_GetCurrentWaveNumber", "wave"),
    ("Level_GetCurrentWaveTotal", "wave_total"),
    ("Level_GetNextWaveTime", "next_wave_time"),
    ("Level_GetCurrentIsEndless", "endless"),
    ("Level_GetCurrentWon", "won"),
    ("Game_IsPaused", "paused"),
    ("Game_GetSecondsInGame", "seconds"),
]
PLAYER_BINDINGS = [   # binding(i) -> per-player key
    ("Player_GetCash", "cash"),
    ("Net_GetPingMS", "ping_ms"),
    ("Net_GetTimeSinceLastReceived", "since_rx"),
]


class DevError(Exception):
    """The console answered {"ok": false, "error": ...}."""


class ConnectionLost(Exception):
    """The pipe closed: the game exited, crashed, or the console was never started."""


class PipeTransport:
    """Line transport over \\\\.\\pipe\\<name> (Windows)."""

    def __init__(self, name=DEFAULT_PIPE, timeout=10.0):
        path = r"\\.\pipe" + "\\" + name
        deadline = time.time() + timeout
        while True:
            try:
                self.f = open(path, "r+b", buffering=0)
                break
            except OSError as e:
                if time.time() > deadline:
                    raise ConnectionLost("cannot open %s (%s). Is the game running with [dev] enabled=1?" % (path, e))
                time.sleep(0.2)          # pipe busy between clients, or game still starting
        self.buf = b""

    def send(self, line):
        try:
            self.f.write(line.encode("utf-8") + b"\n")
        except OSError as e:
            raise ConnectionLost(str(e))

    def recv(self):
        while b"\n" not in self.buf:
            try:
                chunk = self.f.read(65536)
            except OSError as e:
                raise ConnectionLost(str(e))
            if not chunk:
                raise ConnectionLost("pipe closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", "replace")

    def close(self):
        try:
            self.f.close()
        except OSError:
            pass


def quote(arg):
    """Python value -> one console token (see dev_split / dev_token_gval)."""
    if arg is True:
        return "true"
    if arg is False:
        return "false"
    if arg is None:
        return "null"
    if isinstance(arg, (int, float)):
        if arg != arg or arg in (float("inf"), float("-inf")):
            raise ValueError("NaN/inf cannot be sent")
        return repr(arg)
    s = str(arg).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return '"%s"' % s


class Client:
    def __init__(self, transport):
        self.t = transport

    def raw(self, line):
        """Sends a line, returns the parsed reply (ok or not)."""
        self.t.send(line)
        return json.loads(self.t.recv())

    def cmd(self, *tokens):
        r = self.raw(" ".join(tokens))
        if not r.get("ok"):
            raise DevError(r.get("error", "unknown error"))
        return r

    def hello(self):
        return self.cmd("hello")

    def call(self, binding, *args):
        """Calls a UI script binding; returns its value (None if it returned nothing)."""
        r = self.cmd("call", binding, *(quote(a) for a in args))
        return r["result"].get("value") if r.get("returned") else None

    def session(self):
        return self.cmd("session")["session"]

    def timescale(self, x):
        return self.cmd("timescale", quote(float(x)))["timescale"]

    def read(self, addr, n):
        return bytes.fromhex(self.cmd("read", hex(addr), str(n))["hex"])

    def log(self, text):
        self.cmd("log", quote(text))

    def state(self):
        """One snapshot. A binding that errors is reported in st["errors"], not raised."""
        st = {"time": round(time.time(), 3), "errors": {}}
        st["ticks"] = self.cmd("ping")["ticks"]
        for name, key in GLOBAL_BINDINGS:
            try:
                st[key] = self.call(name)
            except DevError as e:
                st[key] = None
                st["errors"][name] = str(e)
        n = st.get("player_count")
        n = int(n) if isinstance(n, (int, float)) and 0 < n <= 16 else 1
        st["players"] = []
        for i in range(n):
            p = {"index": i}
            for name, key in PLAYER_BINDINGS:
                try:
                    p[key] = self.call(name, i)
                except DevError as e:
                    p[key] = None
                    st["errors"]["%s(%d)" % (name, i)] = str(e)
            st["players"].append(p)
        try:
            st["session"] = self.session()
        except DevError as e:
            st["session"] = None
            st["errors"]["session"] = str(e)
        if not st["errors"]:
            del st["errors"]
        return st


# ------------------------------------------------------------------ flattening / CSV
CSV_FIELDS = ["time", "ticks", "wave", "wave_total", "next_wave_time", "player_count", "seconds",
              "paused", "won"]


def flat(st, max_players=8):
    row = {k: st.get(k) for k in CSV_FIELDS}
    for i in range(max_players):
        p = st["players"][i] if i < len(st.get("players", [])) else {}
        for _, key in PLAYER_BINDINGS:
            row["p%d_%s" % (i, key)] = p.get(key)
    return row


def csv_fields(max_players=8):
    return CSV_FIELDS + ["p%d_%s" % (i, k) for i in range(max_players) for _, k in PLAYER_BINDINGS]


# ------------------------------------------------------------------ soak test
class Soak:
    """Drives a high-round run and flags disconnects. Pure logic over a Client + clock,
    so it is unit-tested with a fake console (tests/python/test_devcon.py)."""

    def __init__(self, client, until_wave=None, ready_up=False, silence_s=10.0, max_minutes=None,
                 interval=1.0, timescale=None, out=None, clock=time.time, sleep=time.sleep, echo=print):
        self.c, self.until_wave, self.ready_up = client, until_wave, ready_up
        self.silence_s, self.interval, self.timescale = silence_s, interval, timescale
        self.max_s = max_minutes * 60 if max_minutes else None
        self.out, self.clock, self.sleep, self.echo = out, clock, sleep, echo
        self.events = []
        self.start_players = None
        self.last_wave = None

    def event(self, kind, **kw):
        e = dict(kind=kind, time=round(self.clock(), 3), **kw)
        self.events.append(e)
        self.echo("[soak] %s %s" % (kind, json.dumps(kw, sort_keys=True)))
        if self.out:
            self.out.write(json.dumps(e) + "\n")
            self.out.flush()
        return e

    def check(self, st):
        """Inspects one snapshot; records events. Returns a reason to stop, or None."""
        n = st.get("player_count")
        if self.start_players is None and isinstance(n, (int, float)):
            self.start_players = n
        elif isinstance(n, (int, float)) and self.start_players is not None and n < self.start_players:
            self.event("disconnect", reason="player_count", was=self.start_players, now=n, wave=st.get("wave"))
            self.start_players = n
        me = st.get("my_index")
        for p in st.get("players", []):
            rx = p.get("since_rx")
            if p["index"] != me and isinstance(rx, (int, float)) and rx > self.silence_s:
                self.event("silent_peer", player=p["index"], since_rx=rx, wave=st.get("wave"))
        w = st.get("wave")
        if w != self.last_wave:
            if self.last_wave is not None:
                self.event("wave", wave=w, players=n, seconds=st.get("seconds"))
            self.last_wave = w
        if st.get("won"):
            return "won"
        if self.until_wave is not None and isinstance(w, (int, float)) and w >= self.until_wave:
            return "reached wave %s" % w
        return None

    def run(self):
        t0 = self.clock()
        if self.timescale:
            self.c.timescale(self.timescale)
            self.event("timescale", value=self.timescale)
        self.c.log("soak start until_wave=%s" % self.until_wave)
        reason = None
        try:
            while reason is None:
                try:
                    st = self.c.state()
                except ConnectionLost as e:
                    self.event("connection_lost", error=str(e))
                    reason = "connection lost"
                    break
                if self.out:
                    self.out.write(json.dumps(dict(kind="state", **st)) + "\n")
                reason = self.check(st)
                if reason:
                    break
                if self.ready_up and not st.get("paused") and isinstance(st.get("next_wave_time"), (int, float)) \
                        and st["next_wave_time"] > 0:
                    try:
                        self.c.call("Level_ReadyUp", True)
                    except DevError as e:
                        self.event("ready_up_failed", error=str(e))
                if self.max_s is not None and self.clock() - t0 >= self.max_s:
                    reason = "time limit"
                    break
                self.sleep(self.interval)
        except KeyboardInterrupt:
            reason = "interrupted"
        finally:
            if self.timescale and self.timescale != 1:
                try:
                    self.c.timescale(1)
                except (DevError, ConnectionLost):
                    pass
        self.event("end", reason=reason, disconnects=sum(e["kind"] == "disconnect" for e in self.events))
        try:
            self.c.log("soak end: %s" % reason)
        except (DevError, ConnectionLost):
            pass
        return reason


# ------------------------------------------------------------------ CLI
def repl(c):
    print("omd2mp dev console. Raw commands; 'state' for a snapshot; Ctrl+Z/Ctrl+D or 'quit' to exit.")
    while True:
        try:
            line = input("dev> ").strip()
        except (EOFError, KeyboardInterrupt):
            return
        if line in ("quit", "exit"):
            return
        if not line:
            continue
        try:
            r = c.state() if line == "state" else c.raw(line)
            print(json.dumps(r, indent=2))
        except ConnectionLost as e:
            print("connection lost:", e)
            return
        except ValueError as e:
            print("bad reply:", e)


def main(argv=None, transport=None):
    ap = argparse.ArgumentParser(description="omd2mp developer console client")
    ap.add_argument("--pipe", default=DEFAULT_PIPE)
    sub = ap.add_subparsers(dest="mode")
    sub.add_parser("state")
    w = sub.add_parser("watch")
    w.add_argument("--interval", type=float, default=1.0)
    w.add_argument("--csv")
    w.add_argument("--count", type=int, help="stop after this many samples")
    s = sub.add_parser("soak")
    s.add_argument("--timescale", type=float)
    s.add_argument("--until-wave", type=int)
    s.add_argument("--ready-up", action="store_true", help="press ready whenever a wave countdown is running")
    s.add_argument("--silence", type=float, default=10.0, help="seconds without packets before a peer is flagged")
    s.add_argument("--max-minutes", type=float)
    s.add_argument("--interval", type=float, default=1.0)
    s.add_argument("--log", help="JSONL file for states and events")
    argv = list(sys.argv[1:] if argv is None else argv)
    pipe = DEFAULT_PIPE
    if len(argv) >= 2 and argv[0] == "--pipe":
        pipe, argv = argv[1], argv[2:]
    if argv and argv[0] not in ("state", "watch", "soak") and not argv[0].startswith("-"):
        c = Client(transport or PipeTransport(pipe))      # raw command: devcon.py call Net_PlayerCount
        r = c.raw(" ".join(argv))
        print(json.dumps(r))
        return 0 if r.get("ok") else 1
    args = ap.parse_args((["--pipe", pipe] if pipe != DEFAULT_PIPE else []) + argv)

    c = Client(transport or PipeTransport(args.pipe))
    if args.mode is None:
        repl(c)
        return 0
    if args.mode == "state":
        print(json.dumps(c.state(), indent=2))
        return 0
    if args.mode == "watch":
        f = open(args.csv, "a", newline="") if args.csv else None
        wr = None
        if f:
            wr = csv.DictWriter(f, fieldnames=csv_fields())
            if f.tell() == 0:
                wr.writeheader()
        try:
            i = 0
            while args.count is None or i < args.count:
                st = c.state()
                print("t=%s wave=%s/%s players=%s cash=%s" % (st["ticks"], st.get("wave"), st.get("wave_total"),
                      st.get("player_count"), [p.get("cash") for p in st["players"]]))
                if wr:
                    wr.writerow(flat(st))
                    f.flush()
                i += 1
                if args.count is None or i < args.count:
                    time.sleep(args.interval)
        except KeyboardInterrupt:
            pass
        finally:
            if f:
                f.close()
        return 0
    if args.mode == "soak":
        out = open(args.log, "a") if args.log else None
        try:
            reason = Soak(c, until_wave=args.until_wave, ready_up=args.ready_up, silence_s=args.silence,
                          max_minutes=args.max_minutes, interval=args.interval, timescale=args.timescale,
                          out=out).run()
        finally:
            if out:
                out.close()
        return 0 if reason in ("won",) or (reason or "").startswith("reached") else 2
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ConnectionLost, DevError) as e:
        print("error:", e, file=sys.stderr)
        sys.exit(1)
