"""RP6 Gyro (Decky backend): per-game gyro profiles for rp6gyrod, calibration and Steam layout generation.

rp6gyrod (system service rp6-deckproxy) picks the profile of the running game (profiles/<appid>.conf, else
profiles/default.conf) on top of the global proxy.conf and re-reads both within a second. The IMU rate lives in
imu.env and needs the user service rp6-motion (rp6imu) restarted.
"""
import asyncio
import glob
import json
import os
import re
import subprocess
import time

import decky

ENV_FILE = "/etc/rp6-gyro.env"


def _install_dir():
    """Install directory: RP6_GYRO_DIR from /etc/rp6-gyro.env (written by install.sh)."""
    try:
        for line in open(ENV_FILE):
            if line.startswith("RP6_GYRO_DIR="):
                return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return os.environ.get("RP6_GYRO_DIR", "/var/lib/rp6-gyro")


WORK = _install_dir()
CONF = f"{WORK}/motion/proxy.conf"
PROFILES = f"{WORK}/motion/profiles"
IMU_ENV = f"{WORK}/motion/imu.env"
AIMLAB_TARGET = f"{WORK}/aimlab/target.conf"
STATUS = "/run/rp6-gyro/status.json"
USER = decky.DECKY_USER
HOME = decky.DECKY_USER_HOME
STEAM = f"{HOME}/.local/share/Steam"

# Settings stored per profile (everything the menu offers except the global ones).
PROFILE_BOOL = ("enabled", "invert_pitch", "invert_yaw", "invert_roll")
PROFILE_FLOAT = {"sensitivity": (0.05, 4.0), "smooth_threshold": (0.0, 10.0), "tighten_threshold": (0.0, 10.0),
                 "antideadzone_dps": (0.0, 60.0)}
GLOBAL_BOOL = ("rotation_accel",)
MODES = ("stick", "mouse")
# Defaults per mode; the values come from the Aim Lab fit (stick) and the measured Steam gyro->mouse response.
MODE_DEFAULTS = {"stick": {"sensitivity": 0.56, "antideadzone_dps": 11.5},
                 "mouse": {"sensitivity": 0.21, "antideadzone_dps": 0.0}}
DEFAULT_ID = "default"


# ------------------------------------------------------------------------------------------ file helpers
def read_kv(path):
    raw = {}
    try:
        for line in open(path, encoding="utf-8"):
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                raw[k.strip()] = v.strip()
    except OSError:
        pass
    return raw


def write_kv(path, raw, header):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(f"# {header}\n" + "".join(f"{k}={v}\n" for k, v in raw.items()))
    st = os.stat(os.path.dirname(path))
    os.chown(tmp, st.st_uid, st.st_gid)
    os.replace(tmp, path)


def profile_path(pid):
    pid = str(pid)
    if pid != DEFAULT_ID and not pid.isdigit():
        raise ValueError(f"bad profile id {pid!r}")
    return f"{PROFILES}/{pid}.conf"


def profile_view(raw):
    """Profile file -> typed dict for the UI."""
    p = {"mode": raw.get("mode", "stick") if raw.get("mode") in MODES else "stick", "name": raw.get("name", ""),
         "gamepad_support": int(raw.get("gamepad_support", "-1") or -1)}
    for k in PROFILE_BOOL:
        p[k] = raw.get(k, "1") not in ("0", "false", "")
    p["compensation"] = raw.get("compensation", "2") != "0"          # on = fixed drift offset
    for k in PROFILE_FLOAT:
        try:
            p[k] = float(raw.get(k, MODE_DEFAULTS[p["mode"]].get(k, 0.0)))
        except ValueError:
            p[k] = 0.0
    return p


def new_profile(mode, name="", support=-1):
    """Defaults for a game: mode-specific gain, axis inversion copied from the user's global settings."""
    base = read_kv(CONF)
    raw = {"name": name, "gamepad_support": str(support), "mode": mode, "enabled": "1", "compensation": "2"}
    for k in ("invert_pitch", "invert_yaw", "invert_roll"):
        raw[k] = base.get(k, "1")
    for k in ("smooth_threshold", "tighten_threshold"):
        raw[k] = "0.00"
    for k, v in MODE_DEFAULTS[mode].items():
        raw[k] = f"{v:.2f}"
    return raw


def ensure_profile(pid, support, name=""):
    """Create a game's profile on first sight: gamepad for full controller support, mouse otherwise."""
    path = profile_path(pid)
    if not os.path.exists(path):
        write_kv(path, new_profile("stick" if support == 2 else "mouse", name, support), f"RP6 Gyro profile {pid}")
    return profile_view(read_kv(path))


def list_profiles():
    out = []
    for p in sorted(glob.glob(f"{PROFILES}/*.conf")):
        pid = os.path.basename(p)[:-5]
        out.append({"id": pid, "name": read_kv(p).get("name", "") or ("Default" if pid == DEFAULT_ID else pid)})
    return out


# ------------------------------------------------------------------------------------------ global state
def read_rate():
    try:
        for line in open(IMU_ENV):
            if line.startswith("LIBSSC_RATE_gyro="):
                return int(float(line.split("=", 1)[1]))
    except (OSError, ValueError):
        pass
    return 400


def systemctl(*args, user=False, timeout=30):
    cmd = ["systemctl"] + (["--user", "-M", f"{USER}@"] if user else []) + list(args)
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def state(pid=None):
    try:
        st = json.load(open(STATUS))
        st["stale"] = time.time() - st.get("time", 0) > 6
    except (OSError, ValueError):
        st = None
    pid = str(pid or DEFAULT_ID)
    if pid == DEFAULT_ID and not os.path.exists(profile_path(DEFAULT_ID)):
        write_kv(profile_path(DEFAULT_ID), new_profile("stick", "Default"), "RP6 Gyro default profile")
    path = profile_path(pid)
    return {
        "profile_id": pid,
        "profile": profile_view(read_kv(path)) if os.path.exists(path) else None,
        "profiles": list_profiles(),
        "global": {"rotation_accel": read_kv(CONF).get("rotation_accel", "1") != "0"},
        "rate": read_rate(),
        "status": st,
        "services": {"hexagonrpcd": systemctl("is-active", "rp6-hexagonrpcd"),
                     "proxy": systemctl("is-active", "rp6-deckproxy"),
                     "imu": systemctl("is-active", "rp6-motion", user=True)},
    }


# ------------------------------------------------------------------------------------------ actions
def set_profile_setting(pid, key, value):
    path = profile_path(pid)
    raw = read_kv(path)
    if key == "mode":
        if value not in MODES:
            raise ValueError("bad mode")
        if value != raw.get("mode"):             # switching mode loads that mode's gain defaults
            raw.update({k: f"{v:.2f}" for k, v in MODE_DEFAULTS[value].items()})
        raw[key] = value
    elif key == "compensation":
        raw[key] = "2" if value else "0"
    elif key in PROFILE_BOOL:
        raw[key] = "1" if value else "0"
    elif key in PROFILE_FLOAT:
        lo, hi = PROFILE_FLOAT[key]
        raw[key] = f"{min(hi, max(lo, float(value))):.2f}"
    else:
        raise ValueError(f"unknown setting {key}")
    write_kv(path, raw, f"RP6 Gyro profile {pid}")


def set_global(key, value):
    if key not in GLOBAL_BOOL:
        raise ValueError(f"unknown setting {key}")
    raw = read_kv(CONF)
    raw[key] = "1" if value else "0"
    write_kv(CONF, raw, "rp6gyrod global settings (re-read within a second)")
    if key == "rotation_accel":      # the uinput device is created at start-up
        systemctl("restart", "rp6-deckproxy")


def calibrate_drift():
    raw = read_kv(CONF)
    raw["calibrate_seq"] = str(int(raw.get("calibrate_seq", "0") or 0) + 1)
    write_kv(CONF, raw, "rp6gyrod global settings (re-read within a second)")


def set_rate(hz):
    hz = 200 if int(hz) <= 200 else 400   # accel and gyro share the QMI8658 data rate
    tmp = IMU_ENV + ".tmp"
    with open(tmp, "w") as f:
        f.write(f"LIBSSC_RATE_gyro={hz}\nLIBSSC_RATE_accel={hz}\n")
    st = os.stat(os.path.dirname(IMU_ENV))
    os.chown(tmp, st.st_uid, st.st_gid)
    os.replace(tmp, IMU_ENV)
    systemctl("restart", "rp6-motion", user=True)


def prepare_aimlab(pid):
    """Tell Aim Lab which profile its calibration result is written to."""
    write_kv(AIMLAB_TARGET, {"profile": pid}, "Aim Lab target profile")


# ------------------------------------------------------------------------------------------ Steam layouts
LAYOUT_TITLES = {"stick": "RP6 Gamepad + Gyro (stick)", "mouse": "RP6 Gamepad + Gyro (mouse)", "kbm": "RP6 Keyboard + Mouse + Gyro"}
TEMPLATES = {"stick": "controller_neptune_gamepad_joystick.vdf", "mouse": "controller_neptune_gamepad_joystick.vdf",
             "kbm": "controller_neptune_wasd.vdf"}


def _gyro_group(mode):
    settings = '\t\t\t"gyro_ratchet_button_mask"\t\t"0"\n'
    if mode == "gyro_to_mouse":   # no Steam-side attenuation: the daemon owns the response curve
        settings += '\t\t\t"gyro_button_invert"\t\t"2"\n\t\t\t"gyro_speed_deadzone"\t\t"0"\n\t\t\t"gyro_precision_speed"\t\t"0"\n'
    return ('\t"group"\n\t{\n\t\t"id"\t\t"18"\n\t\t"mode"\t\t"%s"\n\t\t"name"\t\t""\n\t\t"description"\t\t""\n'
            '\t\t"inputs"\n\t\t{\n\t\t}\n\t\t"settings"\n\t\t{\n%s\t\t}\n\t}\n' % (mode, settings))


def layout_name(kind):
    return f"rp6 gyro {kind}_0"


def build_layout(kind, appid):
    """Generate a handheld layout from Steam's stock template plus a gyro group (no third-party layout is shipped)."""
    s = open(f"{STEAM}/controller_base/templates/{TEMPLATES[kind]}", encoding="utf-8-sig").read()
    s = s.replace('"controller_type"\t\t"controller_neptune"', '"controller_type"\t\t"controller_steamos_handheld"')
    head = ('\t"title"\t\t"%s"\n\t"description"\t\t"Generated by RP6 Gyro"\n\t"url"\t\t"usercloud://%s/%s"\n'
            '\t"export_type"\t\t"personal_local"\n' % (LAYOUT_TITLES[kind], appid, layout_name(kind)))
    s = re.sub(r'\t"title"\s+"#Title"\n\t"description"\s+"#Description"\n', head, s, count=1)
    if kind == "kbm":   # eight-way left stick so WASD gives diagonals
        i = s.index('"id"\t\t"3"\n\t\t"mode"\t\t"dpad"')
        end = s.index('\t"group"', i)
        j = s.rindex('"settings"', i, end)
        k = s.index("}", j)
        s = s[:k] + '\t"layout"\t\t"0"\n\t\t\t"overlap_region"\t\t"30000"\n\t\t' + s[k:]
    pre = s.index('\t"preset"')
    s = s[:pre] + _gyro_group("gyro_to_joystick" if kind == "stick" else "gyro_to_mouse") + s[pre:]
    return s.replace('"group_source_bindings"\n\t\t{', '"group_source_bindings"\n\t\t{\n\t\t\t"18"\t\t"gyro active"', 1)


def write_layout(kind, appid):
    """Write the layout file for an app and return the URL Steam's UI needs to select it."""
    if kind not in LAYOUT_TITLES:
        raise ValueError("bad layout kind")
    cfgs = glob.glob(f"{STEAM}/steamapps/common/Steam Controller Configs/*/config")
    if not cfgs:
        raise RuntimeError("Steam controller config directory not found")
    d = f"{cfgs[0]}/{int(appid)}"
    os.makedirs(d, exist_ok=True)
    path = f"{d}/{layout_name(kind)}.vdf"
    with open(path, "w", encoding="utf-8") as f:
        f.write(build_layout(kind, int(appid)))
    st = os.stat(cfgs[0])
    for p in (d, path):
        os.chown(p, st.st_uid, st.st_gid)
    return f"usercloud://{int(appid)}/{layout_name(kind)}"


def safe_state(pid=None):
    """state() that reports problems (missing install dir, unreadable files) instead of raising."""
    if not os.path.isdir(WORK):
        return {"error": f"Install directory {WORK} not found (SD card not mounted?). Run install.sh again."}
    try:
        return state(pid)
    except (OSError, ValueError) as e:
        return {"error": f"Cannot read gyro settings: {e}"}


class Plugin:
    async def get_state(self, pid=None):
        return await asyncio.to_thread(safe_state, pid)

    async def ensure_profile(self, pid, support, name=""):
        await asyncio.to_thread(ensure_profile, pid, int(support), name)
        return await asyncio.to_thread(safe_state, pid)

    async def set_profile_setting(self, pid, key, value):
        await asyncio.to_thread(set_profile_setting, pid, key, value)
        return await asyncio.to_thread(safe_state, pid)

    async def set_global(self, pid, key, value):
        await asyncio.to_thread(set_global, key, value)
        return await asyncio.to_thread(safe_state, pid)

    async def calibrate_drift(self, pid=None):
        await asyncio.to_thread(calibrate_drift)
        return await asyncio.to_thread(safe_state, pid)

    async def prepare_aimlab(self, pid):
        await asyncio.to_thread(prepare_aimlab, pid)

    async def write_layout(self, kind, appid):
        return await asyncio.to_thread(write_layout, kind, appid)

    async def set_rate(self, pid, hz):
        await asyncio.to_thread(set_rate, hz)
        return await asyncio.to_thread(safe_state, pid)

    async def restart_services(self, pid=None):
        await asyncio.to_thread(systemctl, "restart", "rp6-motion", user=True)
        await asyncio.to_thread(systemctl, "restart", "rp6-deckproxy")
        return await asyncio.to_thread(safe_state, pid)

    async def _main(self):
        decky.logger.info("RP6 Gyro backend loaded (dir %s)", WORK)

    async def _unload(self):
        pass
