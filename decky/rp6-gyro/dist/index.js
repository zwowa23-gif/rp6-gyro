// RP6 Gyro: Decky panel for the Retroid Pocket 6 gyroscope (rp6gyrod).
// Hand-written against Decky's globals (SP_REACT, DFL) and loader API v2, no build step.
const manifest = { name: "RP6 Gyro" };
const internalAPIConnection = window.__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit;
if (!internalAPIConnection) {
    throw new Error("[RP6 Gyro] Decky loader API is not initialized");
}
let api;
try {
    api = internalAPIConnection.connect(2, manifest.name);
}
catch {
    api = internalAPIConnection.connect(1, manifest.name);
}
const call = (...args) => api.call(...args);

const h = SP_REACT.createElement;
const { useState, useEffect, useRef } = SP_REACT;
const { PanelSection, PanelSectionRow, ToggleField, SliderField, ButtonItem, DropdownItem, Field } = DFL;

const SERVICE_NAMES = { hexagonrpcd: "ADSP (hexagonrpcd)", proxy: "Proxy (rp6gyrod)", imu: "Sensors (rp6imu)" };
const AIMLAB_NAME = "RP6 Aim Lab";
const DEFAULT_ID = "default";
const CTRL_PORT_HANDHELD = 15;     // Steam Input controller type of the handheld layout set

const appOverview = (id) => {
    try { return window.appStore.GetAppOverviewByAppID(Number(id)); } catch { return null; }
};
const findAimLab = () => {
    try {
        const a = (window.appStore.allApps || []).find((o) => o.display_name === AIMLAB_NAME);
        return a ? a.appid : null;
    } catch { return null; }
};
const selectLayout = (appid, url) => SteamClient.Input.SetSelectedConfigForApp(Number(appid), CTRL_PORT_HANDHELD, url, false, 2);

function statusText(st) {
    if (!st || st.stale) {
        return "No data from rp6gyrod";
    }
    if (st.imu_hz < 10) {
        return "Sensors silent: SSC is not up yet or rp6imu is restarting";
    }
    const g = st.gyro_dps.map((v) => v.toFixed(1)).join(" / ");
    const cal = st.calibrating ? "calibrating…" : !st.calibrated ? "drift not measured: put the console on a table for 2 s"
        : `drift measured ${st.cal_age < 60 ? Math.round(st.cal_age) + " s" : Math.round(st.cal_age / 60) + " min"} ago`;
    return `IMU ${Math.round(st.imu_hz)} Hz · ${st.still ? "still" : `deg/s ${g}`} · ${cal}`;
}

// Which Steam layout matches a profile: gamepad games drive the camera stick (or a mouse), games without
// full gamepad support get keyboard + mouse.
function layoutKind(profile) {
    if (profile.mode === "stick") return "stick";
    return profile.gamepad_support === 2 ? "mouse" : "kbm";
}
const LAYOUT_TEXT = {
    stick: "Gamepad layout, gyro drives the right stick",
    mouse: "Gamepad layout, gyro drives the mouse",
    kbm: "Keyboard + mouse layout (left stick = WASD, gyro = mouse)",
};

function Content() {
    const [state, setState] = useState(null);
    const [pick, setPick] = useState(null);          // profile chosen by hand; null = follow the running game
    const [busy, setBusy] = useState(false);
    const [note, setNote] = useState("");
    const timers = useRef({});
    const pickRef = useRef(null);
    pickRef.current = pick;

    // The daemon reports the running game; the profile follows it unless the user picked another one.
    const effective = (st) => pickRef.current || (st && st.status && st.status.appid ? String(st.status.appid) : DEFAULT_ID);
    const refresh = () => {
        call("get_state", pickRef.current || DEFAULT_ID).then((first) => {
            if (first.error) return setState(first);
            const id = effective(first);
            if (id === DEFAULT_ID || (first.profile_id === id && first.profile)) {
                return setState(first);
            }
            // first sight of a game: create its profile (gamepad for full controller support, mouse otherwise)
            const ov = appOverview(id);
            return call("ensure_profile", id, ov ? ov.xbox_controller_support : -1, ov ? ov.display_name : "").then(setState);
        }).catch(() => { });
    };
    useEffect(() => {
        refresh();
        const t = setInterval(refresh, 1500);
        return () => clearInterval(t);
    }, []);

    if (!state) {
        return h(PanelSection, { title: "RP6 Gyro" }, h(PanelSectionRow, null, h(Field, { label: "Loading…" })));
    }
    if (state.error) {
        return h(PanelSection, { title: "RP6 Gyro" }, h(PanelSectionRow, null, h(Field, { label: "Gyro backend problem", description: state.error })));
    }
    const pid = effective(state);
    const p = state.profile && state.profile_id === pid ? state.profile : null;
    const st = state.status;
    const running = st && st.appid ? String(st.appid) : null;
    const gameName = (id) => {
        const f = state.profiles.find((x) => x.id === id);
        const ov = appOverview(id);
        return (ov && ov.display_name) || (f && f.name) || id;
    };
    const run = (method, ...args) => {
        setBusy(true);
        return call(method, ...args).then(setState).catch(() => { }).finally(() => setBusy(false));
    };
    const set = (key, value) => {
        if (!p) return;
        setState({ ...state, profile: { ...p, [key]: value } });
        call("set_profile_setting", pid, key, value).then(setState).catch(() => { });
    };
    const setSlider = (key) => (value) => {
        if (!p) return;
        setState({ ...state, profile: { ...p, [key]: value } });
        clearTimeout(timers.current[key]);          // sliders fire continuously while dragged
        timers.current[key] = setTimeout(() => call("set_profile_setting", pid, key, value).catch(() => { }), 250);
    };

    const options = [{ data: DEFAULT_ID, label: "Default (games without a profile)" }];
    if (running && !state.profiles.some((x) => x.id === running)) {
        options.push({ data: running, label: `${gameName(running)} (running)` });
    }
    state.profiles.filter((x) => x.id !== DEFAULT_ID).forEach((x) => {
        options.push({ data: x.id, label: x.id === running ? `${gameName(x.id)} (running)` : gameName(x.id) });
    });
    const services = Object.entries(state.services)
        .map(([k, v]) => `${SERVICE_NAMES[k] || k}: ${v === "active" ? "running" : v}`).join(" · ");
    const isGame = pid !== DEFAULT_ID;
    const mouse = p && p.mode === "mouse";
    const aimLab = findAimLab();

    const applyLayout = () => {
        const kind = layoutKind(p);
        setBusy(true);
        call("write_layout", kind, pid).then((url) => { selectLayout(pid, url); setNote(`Applied: ${LAYOUT_TEXT[kind]}`); })
            .catch((e) => setNote("Layout failed: " + e)).finally(() => setBusy(false));
    };
    const fullCalibration = () => {
        // Aim Lab writes its result into this profile; it needs the layout of the profile's mode.
        setBusy(true);
        call("prepare_aimlab", pid)
            .then(() => call("write_layout", p.mode === "mouse" ? "mouse" : "stick", aimLab))
            .then((url) => { selectLayout(aimLab, url); })
            .then(() => {
                const gid = (BigInt(aimLab) << 32n) | 0x02000000n;
                SteamClient.URL.ExecuteSteamURL(`steam://rungameid/${gid}`);
            })
            .catch((e) => setNote("Could not start Aim Lab: " + e)).finally(() => setBusy(false));
    };

    return h("div", null,
        h(PanelSection, { title: "Status" },
            h(PanelSectionRow, null, h(Field, { label: statusText(st), description: services, bottomSeparator: "none" }))),
        h(PanelSection, { title: "Profile" },
            h(PanelSectionRow, null, h(DropdownItem, {
                label: "Game profile",
                description: running ? `Following the running game; pick another to edit it` : "No game running",
                rgOptions: options, selectedOption: pid,
                onChange: (o) => setPick(o.data === running ? null : o.data),
            })),
            p && h(PanelSectionRow, null, h(DropdownItem, {
                label: "Gyro drives",
                description: mouse ? "Mouse: games without gamepad support" : "Gamepad stick: games with gamepad support",
                rgOptions: [{ data: "stick", label: "Gamepad stick" }, { data: "mouse", label: "Mouse" }],
                selectedOption: p.mode, onChange: (o) => set("mode", o.data),
            }))),
        p && h(PanelSection, { title: "Gyro" },
            h(PanelSectionRow, null, h(ToggleField, {
                label: "Gyro enabled", description: "Off: Steam receives zeros instead of motion",
                checked: p.enabled, onChange: (v) => set("enabled", v),
            })),
            h(PanelSectionRow, null, h(SliderField, {
                label: "Sensitivity", description: "Multiplier applied before Steam's own gyro sensitivity",
                value: p.sensitivity, min: 0.05, max: 3, step: 0.01, showValue: true, editableValue: true,
                onChange: setSlider("sensitivity"),
            })),
            !mouse && h(PanelSectionRow, null, h(SliderField, {
                label: "Stick anti-dead-zone, deg/s",
                description: "Gyro as stick only: speed added once you move, so small turns clear the game's stick dead zone. 0 = off",
                value: p.antideadzone_dps, min: 0, max: 40, step: 0.5, showValue: true,
                onChange: setSlider("antideadzone_dps"),
            })),
            h(PanelSectionRow, null, h(ToggleField, {
                label: "Drift compensation",
                description: "One fixed offset, measured once at rest or with the calibrate button. Off: raw signal",
                checked: p.compensation, onChange: (v) => set("compensation", v),
            }))),
        p && h(PanelSection, { title: "Axis inversion" },
            h(PanelSectionRow, null, h(ToggleField, { label: "Pitch (up/down)", checked: p.invert_pitch, onChange: (v) => set("invert_pitch", v) })),
            h(PanelSectionRow, null, h(ToggleField, { label: "Yaw (left/right)", checked: p.invert_yaw, onChange: (v) => set("invert_yaw", v) })),
            h(PanelSectionRow, null, h(ToggleField, { label: "Roll (tilt sideways)", checked: p.invert_roll, onChange: (v) => set("invert_roll", v) }))),
        p && isGame && h(PanelSection, { title: "Steam layout" },
            h(PanelSectionRow, null, h(ButtonItem, {
                layout: "below", disabled: busy, onClick: applyLayout,
                description: `${LAYOUT_TEXT[layoutKind(p)]}. Replaces this game's selected layout`,
            }, "Apply matching layout")),
            note && h(PanelSectionRow, null, h(Field, { label: note, bottomSeparator: "none" }))),
        h(PanelSection, { title: "Calibration" },
            h(PanelSectionRow, null, h(ButtonItem, {
                layout: "below", disabled: busy, onClick: () => run("calibrate_drift", pid),
                description: "Put the console still on a table for 2 seconds",
            }, "Calibrate drift")),
            p && h(PanelSectionRow, null, aimLab
                ? h(ButtonItem, {
                    layout: "below", disabled: busy, onClick: fullCalibration,
                    description: `Rotation test in Aim Lab; fits sensitivity${mouse ? "" : " and anti-dead-zone"} for this profile`,
                }, "Full calibration (Aim Lab)")
                : h(Field, { label: "Full calibration needs RP6 Aim Lab", description: `Add it to Steam as a non-Steam game named "${AIMLAB_NAME}"`, bottomSeparator: "none" }))),
        h(PanelSection, { title: "Advanced" },
            p && h(PanelSectionRow, null, h(SliderField, {
                label: "Smoothing of small movements, deg/s", description: "0 = off. Only movement slower than the threshold is smoothed",
                value: p.smooth_threshold, min: 0, max: 8, step: 0.5, showValue: true, onChange: setSlider("smooth_threshold"),
            })),
            p && h(PanelSectionRow, null, h(SliderField, {
                label: "Tightening, deg/s", description: "0 = off. Below the threshold the speed is scaled down smoothly",
                value: p.tighten_threshold, min: 0, max: 8, step: 0.5, showValue: true, onChange: setSlider("tighten_threshold"),
            })),
            h(PanelSectionRow, null, h(ToggleField, {
                label: "Economy mode (200 Hz)", description: "Half the CPU load, slightly more latency",
                checked: state.rate <= 200, disabled: busy, onChange: (v) => run("set_rate", pid, v ? 200 : 400),
            })),
            h(PanelSectionRow, null, h(ToggleField, {
                label: "Screen auto-rotation", description: "Accelerometer for rotating the desktop",
                checked: state.global.rotation_accel, onChange: (v) => run("set_global", pid, "rotation_accel", v),
            })),
            h(PanelSectionRow, null, h(ButtonItem, { layout: "below", disabled: busy, onClick: () => run("restart_services", pid) }, "Restart gyro services"))));
}

const icon = h("svg", {
    xmlns: "http://www.w3.org/2000/svg", width: "24", height: "24", viewBox: "0 0 24 24", fill: "none",
    stroke: "currentColor", strokeWidth: "2", strokeLinecap: "round", strokeLinejoin: "round",
}, h("circle", { cx: "12", cy: "12", r: "3" }), h("path", { d: "M3 12a9 9 0 0 1 15.5-6.2" }),
    h("path", { d: "M21 12a9 9 0 0 1-15.5 6.2" }), h("path", { d: "M18.5 2.5v3.3h-3.3" }), h("path", { d: "M5.5 21.5v-3.3h3.3" }));

const index = () => ({
    name: manifest.name,
    titleView: h("div", { className: DFL.staticClasses && DFL.staticClasses.Title }, manifest.name),
    content: h(Content, null),
    icon,
});

export { index as default };
