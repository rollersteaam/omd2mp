"""
build_rebalance.py - generate the omd2mp wave-rebalance data mod for N players.

How OMD2 scales waves for co-op (from the Workshop Tool's SourceXML/onslaughts):
  * every <Entity> in a wave spawns in all modes;
  * an <Entity ... Coop=""> spawns ONLY when a second player is connected.
  So vanilla co-op = solo waves + one "extra player's worth" of Coop entities.

For N players we keep the solo entities and multiply each Coop entity's Count by
    extra = (N - 1) * coop_scale
so N=2 with coop_scale=1.0 reproduces vanilla exactly. Gold is rebalanced by the loader's
`economy` patch (NetMoneyFactor = 1/N), which keeps total team gold equal to 2-player.

Edits are textual (regex on the Count attribute of Coop entities) so comments, formatting
and attribute order in Robot's files survive untouched. Only *.onslaught files that change
are written. Doors (<OpenDoor Coop="">, <BangDoor Coop="">) are left alone.

Usage:
  python mods/rebalance/build_rebalance.py --players 4 [--coop-scale 1.0]
         [--src "<Workshop Tool>/build/modtool/SourceXML"] [--out build/mods/omd2mp_4p]
Output layout matches what the game loads for a data mod:
  <out>/mod/game/onslaughts/*.onslaught  and  <out>/rebalance_manifest.json
"""
import argparse, json, math, os, re, sys

DEFAULT_SRC = r"C:\Program Files (x86)\Steam\steamapps\common\Orcs Must Die 2 Workshop Tool\build\modtool\SourceXML"
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

ENTITY = re.compile(r"<Entity\b[^>]*>", re.S)
COOP = re.compile(r"\bCoop\s*=\s*\"[^\"]*\"")
COUNT = re.compile(r"(\bCount\s*=\s*\")([0-9]+)(\")")


def scaled_count(count, players, coop_scale):
    """Coop entity count for `players`. Never below 0; N=2, scale=1 is identity."""
    extra = (players - 1) * coop_scale
    return max(0, int(math.floor(count * extra + 0.5)))


def rebalance_text(text, players, coop_scale=1.0):
    """Return (new_text, stats) with every Coop entity's Count scaled."""
    stats = dict(coop_entities=0, mobs_before=0, mobs_after=0)

    def fix(m):
        tag = m.group(0)
        if not COOP.search(tag):
            return tag
        c = COUNT.search(tag)
        if not c:
            return tag
        old = int(c.group(2))
        new = scaled_count(old, players, coop_scale)
        stats["coop_entities"] += 1
        stats["mobs_before"] += old
        stats["mobs_after"] += new
        return tag[:c.start(2)] + str(new) + tag[c.end(2):]

    return ENTITY.sub(fix, text), stats


def build(src, out, players, coop_scale):
    ons = os.path.join(src, "onslaughts")
    if not os.path.isdir(ons):
        sys.exit(f"no onslaughts folder under {src}")
    dst = os.path.join(out, "mod", "game", "onslaughts")
    os.makedirs(dst, exist_ok=True)
    manifest = dict(players=players, coop_scale=coop_scale, files={})
    for name in sorted(os.listdir(ons)):
        if not name.lower().endswith(".onslaught"):   # Robot ships both .onslaught and .Onslaught
            continue
        raw = open(os.path.join(ons, name), "rb").read()
        text = raw.decode("utf-8", errors="surrogateescape")
        new, st = rebalance_text(text, players, coop_scale)
        if new != text:
            open(os.path.join(dst, name), "wb").write(new.encode("utf-8", errors="surrogateescape"))
            manifest["files"][name] = st
    tot = {k: sum(f[k] for f in manifest["files"].values()) for k in ("coop_entities", "mobs_before", "mobs_after")}
    manifest["totals"] = tot
    with open(os.path.join(out, "rebalance_manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    return manifest


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--players", type=int, default=4)
    ap.add_argument("--coop-scale", type=float, default=1.0,
                    help="Coop extras per additional player (1.0 = each extra player adds what vanilla's 2nd adds)")
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    if not 2 <= a.players <= 16:
        ap.error("--players must be 2..16")
    out = a.out or os.path.join(ROOT, "build", "mods", f"omd2mp_{a.players}p")
    m = build(a.src, out, a.players, a.coop_scale)
    t = m["totals"]
    print(f"{len(m['files'])} onslaught files, {t['coop_entities']} coop entities, "
          f"coop mobs {t['mobs_before']} -> {t['mobs_after']}  ->  {out}")


if __name__ == "__main__":
    main()
