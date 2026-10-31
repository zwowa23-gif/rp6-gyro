"""Pool every rp6aim session: Steam's response (mouse counts per degree of the gyro it RECEIVED), independent of my
sensitivity/inversion at the time, restored from the f-records (out/phys)."""
import glob, math, statistics as st, sys, bisect
D = sys.argv[1] if len(sys.argv) > 1 else "/var/lib/rp6-gyro/aimlab"
TARGET = 1920 / 90.0
edges = [0.5, 1, 2, 4, 8, 16, 32, 64, 1e9]
rows = []                  # per window: (session, speed_true, oyaw, opitch, dx, dy)
sessions = {}
for f in sorted(glob.glob(D + "/session-*.csv")):
    F, W = [], []
    for l in open(f):
        p = l.strip().split(",")
        try:
            if p[0] == "f": F.append(tuple(map(float, p[1:])))      # t, stage, mdx, mdy, ph0, ph1, out0, out1
            elif p[0] == "win": W.append(tuple(map(float, p[1:])))  # t, dx, dy, dyaw, dpitch, speed
        except ValueError: pass
    if len(W) < 10 or len(F) < 20: continue
    ft = [x[0] for x in F]
    def gain(t, ax):                      # out/phys ratio around time t (|phys| large enough)
        a = bisect.bisect_left(ft, t - 8); b = bisect.bisect_left(ft, t + 8)
        num = den = 0.0
        for x in F[a:b]:
            ph, out = x[4 + ax], x[6 + ax]
            if abs(ph) > 3: num += out * ph; den += ph * ph
        return num / den if den > 1e-6 else None
    n = 0
    for t, dx, dy, dyaw, dpitch, sp in W:
        gy, gp = gain(t, 1), gain(t, 0)
        if gy is None or gp is None: continue
        rows.append((f.split("-")[-1][:-4], sp, gy * dyaw, gp * dpitch, dx, dy, gy, gp)); n += 1
    sessions[f.split("-")[-1][:-4]] = n
print("sessions (windows with a gain estimate):", sessions, " total", len(rows))
def K(sel, axis):                         # counts per received degree, signed
    num = sum(r[4 + axis] * r[2 + axis] for r in sel); den = sum(r[2 + axis] ** 2 for r in sel)
    return (num / den if den > 1e-9 else float("nan")), den
print("\nSteam response, counts per degree of the gyro Steam received (negative = mouse moves against the received rotation)")
print("          windows |  yaw: K     |  pitch: K   | dead zone check (K / K_ref)")
ref = [r for r in rows if r[1] >= 16]
kyr, _ = K(ref, 0); kpr, _ = K(ref, 1)
print(f"  ref (>=16 dps) n={len(ref)}  yaw {kyr:+.2f}  pitch {kpr:+.2f}")
for lo, hi in zip(edges, edges[1:]):
    sel = [r for r in rows if lo <= r[1] < hi]
    if len(sel) < 5: continue
    ky, dy_ = K(sel, 0); kp, dp_ = K(sel, 1)
    flag = ""
    if abs(kyr) > 1: flag += f" yaw {ky / kyr:4.2f}"
    if abs(kpr) > 1: flag += f" pitch {kp / kpr:4.2f}"
    print(f"  {lo:5}-{hi if hi < 1e8 else 999:<5} {len(sel):5} | {ky:+8.2f}    | {kp:+8.2f}    |{flag}")
per = {}
for r in rows: per.setdefault(r[0], []).append(r)
print("\nper-session consistency (windows >= 4 dps):")
for s, sel in per.items():
    sel = [r for r in sel if r[1] >= 4]
    if len(sel) < 8: continue
    ky, _ = K(sel, 0); kp, _ = K(sel, 1)
    gy = st.median(r[6] for r in sel); gp = st.median(r[7] for r in sel)
    print(f"  {s}: n={len(sel):3}  K_yaw {ky:+7.2f}  K_pitch {kp:+7.2f}   my gain at the time: yaw {gy:+.3f} pitch {gp:+.3f}")
good = [r for r in rows if r[1] >= 4]
ky, _ = K(good, 0); kp, _ = K(good, 1)
print(f"\nALL runs (>= 4 dps): K_yaw {ky:+.2f}, K_pitch {kp:+.2f} counts per received degree")
kavg = (abs(ky) + abs(kp)) / 2
print(f"=> sensitivity for {TARGET:.2f} counts per TRUE degree (1:1 at 90 deg FOV): {TARGET / kavg:.3f}")

# ---- response curve versus the speed Steam actually RECEIVED (all sessions pooled) ----
print("\nSteam response vs RECEIVED speed (all sessions): counts per received degree, windows of 0.2 s")
redges = [0, 0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 1e9]
useful = [r for r in rows if r[0] != "1793450771"]          # the first run had no mouse output at all (default template)
print("  received dps    n  |  K_yaw  K_pitch |  mean counts/window |  share of windows with zero counts")
for lo, hi in zip(redges, redges[1:]):
    sel = [r for r in useful if lo <= math.hypot(r[2], r[3]) / 0.2 < hi]
    if len(sel) < 8: continue
    ky, _ = K(sel, 0); kp, _ = K(sel, 1)
    zero = sum(1 for r in sel if abs(r[4]) + abs(r[5]) < 0.5) / len(sel)
    mc = st.mean(math.hypot(r[4], r[5]) for r in sel)
    print(f"  {lo:6}-{hi if hi < 1e8 else 999:<6} {len(sel):4} | {ky:+7.1f} {kp:+7.1f} | {mc:8.2f} | {zero:5.0%}")

# ---- robust version: per-window ratio, median per received-speed bin (outliers from touch/stick mouse input do not count) ----
print("\nROBUST (median of per-window ratio -(dx*oyaw+dy*opitch)/(oyaw^2+opitch^2), signed; sessions 2+3 pooled)")
print("  received dps    n | median K | p25..p75 | share with the 'right' sign")
def ratio(r):
    d = r[2] ** 2 + r[3] ** 2
    return -(r[4] * r[2] + r[5] * r[3]) / d if d > 1e-6 else None
allr = []
for lo, hi in zip(redges, redges[1:]):
    sel = [r for r in useful if lo <= math.hypot(r[2], r[3]) / 0.2 < hi]
    vals = [x for x in (ratio(r) for r in sel) if x is not None]
    if len(vals) < 8: continue
    vals.sort(); n = len(vals)
    med, q1, q3 = vals[n // 2], vals[n // 4], vals[3 * n // 4]
    pos = sum(1 for v in vals if v > 0) / n
    allr.append((lo, hi, med, n))
    print(f"  {lo:6}-{hi if hi < 1e8 else 999:<6} {n:4} | {med:+8.1f} | {q1:+7.1f}..{q3:+7.1f} | {pos:4.0%}")
