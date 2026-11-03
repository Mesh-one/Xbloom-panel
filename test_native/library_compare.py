"""Check library.cpp against xbloom-py: parsing cloud recipes and building create/update bodies."""
import json, random, subprocess, sys
sys.path.insert(0, sys.argv[2])
from xbloom.client import _parse_recipe
from xbloom.cloud import recipe_to_cloud_body

exe = sys.argv[1]
rng = random.Random(7)
names = ["MAD ice coffee", "ICE coffee", "  Spaced  ", "Yaye [Anaerobic] {Filter}", "Café Olé", "4 Pour Recipe",
         "قهوة مختصة", "Decaf Colombia Washed"]

def raw_recipe(i):
    n = rng.randint(1, 9)
    dose = rng.choice([12, 15, 18, 20, 22, 25, 15.5])
    pours = []
    for k in range(n):
        pours.append({"tableId": 100 * i + k, "recipeId": i, "theName": rng.choice(["Bloom", f"Pour {k+1}", "Swirl"]),
                      "volume": rng.choice([30, 40, 50, 60, 70.0, 80, 120, 240]),
                      "temperature": rng.choice([20, 85, 90, 93.0, 95, 98]),
                      "pattern": rng.choice([1, 2, 3]), "flowRate": rng.choice([3.0, 3.2, 3.5]),
                      "pausing": rng.randint(0, 59),
                      "isEnableVibrationBefore": rng.choice([1, 2]), "isEnableVibrationAfter": rng.choice([1, 2])})
    total = sum(p["volume"] for p in pours)
    r = {"tableId": rng.choice([803560 + i, str(900000 + i)]), "theName": rng.choice(names), "dose": dose,
         "grandWater": round(total / dose, 1), "grinderSize": rng.choice([20, 36, 45.0, 80]),
         "isSetGrinderSize": rng.choice([1, 2]), "rpm": rng.choice([60, 90, 120]), "pourCount": n, "pourList": pours,
         "cupType": rng.choice([1, 2, 3, 4]), "cupTypeName": "Omni", "isEnableBypassWater": rng.choice([1, 2]),
         "theColor": rng.choice(["#C9D5B8", "#DED9AF", "#b0bde0", "#E4A3A4"]), "adaptedModel": 1,
         "createTimeStamp": 1700000000000 + i * 1234567, "isDefault": 0, "isShortcuts": rng.choice([0, 1, 2]),
         "subSetType": rng.choice([0, 1, 2]), "theSubsetId": rng.choice([0, 5])}
    if r["isEnableBypassWater"] == 1:
        r["bypassTemp"] = rng.choice([80.0, 85, 90])
        r["bypassVolume"] = rng.choice([5.0, 30, 100])
    if rng.random() < 0.2:  # numbers as strings, as some responses carry them
        r["dose"] = str(r["dose"]); r["rpm"] = str(r["rpm"])
    return r

raws = [raw_recipe(i) for i in range(400)]
resp = json.dumps({"result": "success", "list": raws}, ensure_ascii=False)
out = subprocess.run([exe, "parse"], input=resp.encode(), capture_output=True, check=True).stdout.decode().splitlines()
bad = 0
for raw, line in zip(raws, out):
    got = json.loads(line)
    p = got["parsed"]
    ref = _parse_recipe(raw)
    exp_name = ref["name"].strip()
    checks = [
        (p["id"], str(raw["tableId"])), (p["name"], exp_name), (p["dose"], ref["dose_g"]),
        (p["gs"], round(ref["grinder_size"])), (p["gon"], 1 if ref["grinder_size_enabled"] == 1 else 0),
        (p["rpm"], ref["rpm"]), (p["cup"], ref["cup_type"]), (p["bp"], 1 if ref["bypass_water_enabled"] == 1 else 0),
        (p["col"], int(ref["color_hex"][1:], 16)), (p["ct"], ref["created_at_ms"]), (p["sc"], ref["is_shortcut"]),
        (p["st"], ref["subset_type"]), (p["sid"], ref["subset_id"]), (len(p["pours"]), len(ref["pours"])),
    ]
    for a, b in zip(p["pours"], ref["pours"]):
        checks += [(a[0], b["volume_ml"]), (a[1], b["temperature_c"]), (a[2], b["pattern"]), (a[3], b["flow_rate"]),
                   (a[4], b["pause_s"]), (a[5], 1 if b["agitate_before"] == 1 else 0),
                   (a[6], 1 if b["agitate_after"] == 1 else 0), (a[7], b["name"])]
    # body vs xbloom-py's recipe_to_cloud_body
    pref = dict(ref, ratio=f"1:{round(sum(x['volume_ml'] for x in ref['pours']) / ref['dose_g'], 1)}")
    want = recipe_to_cloud_body(pref)
    body = got["body"]
    for k, v in want.items():
        if k == "pourDataJSONStr":
            checks.append((json.loads(body[k]), json.loads(v)))
        else:
            checks.append((body.get(k), v))
    checks += [(body["tableId"], str(raw["tableId"])), (body["memberId"], 7), (body["token"], "t")]
    errs = [(a, b) for a, b in checks if a != b]
    if errs:
        bad += 1
        if bad <= 3:
            print("MISMATCH", raw["tableId"], errs[:5])
print(f"cloud parse + body: {len(raws) - bad}/{len(raws)} identical to xbloom-py")
c = subprocess.run([exe, "cache"], input=resp.encode(), capture_output=True, check=True).stdout.decode().strip()
print("panel file round trip:", c)
e = subprocess.run([exe, "parse"], input=b'{"result":"fail","resultCode":20003,"info":"Please log in again"}',
                   capture_output=True, check=True).stdout.decode().strip()
print("expired-session reply:", e)
sys.exit(0 if bad == 0 and c.startswith("same") and "expired=1" in e else 1)
