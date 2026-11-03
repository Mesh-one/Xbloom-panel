"""Byte-for-byte check of the C++ port against xbloom-py (the reference)."""
import random, struct, subprocess, sys

sys.path.insert(0, sys.argv[2])
from xbloom import ble  # noqa: E402

exe = sys.argv[1]
rng = random.Random(1234)


def rand_recipe():
    cup = rng.choice([2, 3, 3, 1])
    dose = {1: 15.0, 2: rng.choice([12, 15, 15.5, 18]), 3: rng.choice([10, 15, 16.5, 18, 20, 25])}[cup]
    n = rng.randint(1, 9)
    pours = []
    for _ in range(n):
        pours.append(dict(
            volume_ml=float(rng.choice([0, 30, 40, 45.5, 50, 60, 77.7, 100, 127, 128, 200, 240])),
            temperature_c=rng.choice([20, 85, 90, 92, 93, 94, 96, 98]),
            pattern=rng.choice([1, 2, 3]),
            flow_rate=rng.choice([3.0, 3.1, 3.2, 3.3, 3.5]),
            pause_s=rng.randint(0, 59),
            agitate_before=rng.choice([1, 2]),
            agitate_after=rng.choice([1, 2]),
        ))
    gon = rng.choice([1, 1, 0])
    return dict(dose_g=dose, grinder_size=rng.randint(1, 80), rpm=rng.choice([60, 80, 90, 120]),
                cup_type=cup, grinder_size_enabled=gon, pours=pours)


def line(r):
    parts = [r["dose_g"], r["grinder_size"], r["rpm"], r["cup_type"], r["grinder_size_enabled"], len(r["pours"])]
    for p in r["pours"]:
        parts += [p["volume_ml"], p["temperature_c"], p["pattern"], p["flow_rate"], p["pause_s"],
                  1 if p["agitate_before"] == 1 else 0, 1 if p["agitate_after"] == 1 else 0]
    return " ".join(repr(x) if isinstance(x, float) else str(x) for x in parts)


recipes = [rand_recipe() for _ in range(3000)]
out = subprocess.run([exe, "enc"], input="\n".join(line(r) for r in recipes), capture_output=True,
                     text=True, check=True).stdout.split("\n")
bad = 0
for r, got in zip(recipes, out):
    want = " ".join(f.hex() for f in ble.build_brew_frames(r))
    if got.strip() != want:
        bad += 1
        if bad < 4:
            print("MISMATCH", r, "\n got ", got, "\n want", want)
print(f"encode: {len(recipes) - bad}/{len(recipes)} recipes identical")

# Assembler: random frames, packed and split at random points
def nf(code, payload):
    body = bytes([0x58, 0x02, 0x07]) + struct.pack("<H", code) + struct.pack("<I", 12 + len(payload)) + b"\xc1" + payload
    return body + struct.pack("<H", ble._crc16(body))

sessions_in, sessions_want = [], []
for _ in range(400):
    frames = [nf(rng.choice([20501, 40523, 8023, 8002, 40512]), struct.pack("<f", rng.random() * 300))
              for _ in range(rng.randint(1, 8))]
    stream = b"".join(frames)
    if rng.random() < 0.15:
        stream = stream[:-rng.randint(1, 5)]  # truncated tail
    cuts = sorted(rng.sample(range(1, len(stream)), min(len(stream) - 1, rng.randint(0, 4))))
    chunks = [stream[a:b] for a, b in zip([0] + cuts, cuts + [len(stream)])]
    a = ble.NotificationAssembler()
    want = []
    for c in chunks:
        want += [f.hex() for f in a.feed(c)]
    sessions_in.append(chunks)
    sessions_want.append(want)
inp = "\n\n".join("\n".join(c.hex() for c in s) for s in sessions_in) + "\n"
got = subprocess.run([exe, "asm"], input=inp, capture_output=True, text=True, check=True).stdout
got_sessions = [[l for l in s.split("\n") if l] for s in got.split("--\n")]
ok = sum(1 for g, w in zip(got_sessions, sessions_want) if g == w)
print(f"assembler: {ok}/{len(sessions_want)} sessions identical")
g = ble.packets_grind(36, 90)
want = [g[0], g[1], g[2], ble.packet_brewer_set(1, 85), ble.build_brewer_standalone_frame(3.5, 250, 85, 0, 1),
        ble.packet_brewer_temp(92.5), ble.packet_brewer_pattern(2), ble.packet_scale_enter(), ble.packet_quit_grinder(),
        ble.packet_quit_brewer(), ble.packet_quit_scale(), ble.packet_weight_unit("g"), ble.packet_weight_unit("oz"),
        ble.packet_weight_unit("ml")]
got_mod = subprocess.run([exe, "mod"], capture_output=True, text=True, check=True).stdout.split()
mod_ok = sum(1 for a, b in zip(got_mod, want) if a == b.hex())
print(f"module frames: {mod_ok}/{len(want)} identical")
bad += len(want) - mod_ok
sys.exit(0 if bad == 0 and ok == len(sessions_want) else 1)
