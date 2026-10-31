# Architecture notes

## Why the ADSP
The RP6 sensors (QMI8658 accel/gyro, QMC6308 magnetometer) are configured in the Qualcomm SSC registry and powered by the
ADSP; no Linux I2C bus or DT node reaches them. The ADSP's *sensors PD* needs a FastRPC file server from the moment the
ADSP boots: it reads its registry/config through `hexagonrpcd` and **writes** registry files back. Upstream
`hexagonrpcd` is read-only, so the patch adds `fopen` for writing into a scratch directory (`HEXAGONRPCD_WRITE_DIR`),
`fwrite`, `rename`, `remove`, serves `sns_reg_version` and enlarges the input buffer. Once the PD is up, QMI service 400
appears on QRTR node 5 and libssc can stream samples (the libssc patch picks the highest rate up to 400 Hz).

## Frames
libssc reports raw sensor axes. Android's orientation for this board is x:-x, y:-y, z:+z; `rp6gyrod` maps the Android
frame to the Steam Deck frame with `matrix` (default `[[0,-1,0],[1,0,0],[0,0,1]]`: right = -y, up = x, toward player = z).
Units in the Deck report: accel 16384/g at offset 24, gyro 16/(deg/s) at offset 30.

## Deck proxy
InputPlumber exposes a Deck-style controller as hidraw `28de:12f0` with 64-byte unnumbered reports and no IMU. `rp6gyrod`
creates an identical `uhid` copy (`HID_PHYS=rp6-gyro-proxy`), relays input, feature and output reports byte-for-byte,
overwrites the IMU fields, and hides the original from Steam with a udev rule that is active only while the daemon runs
(`/run/rp6-gyro-proxy.active`), so a crash never leaves the console without a controller.

## Compensation
Mode 2 (default): one fixed gyro bias, measured once at first stillness or on request and persisted. No filters, no
dead zones: Steam's own One Euro filter, speed dead zone, precision speed and prediction are disabled via
`steam_dev.cfg` (see below) because they made slow aiming "sticky".

    ~/.local/share/Steam/steam_dev.cfg
    gyro_filter_enabled 0
    gyro_speed_deadzone_enabled 0
    gyro_raw_deadzone 0
    gyro_precision_speed_enabled 0
    gyro_prediction_enabled 0

(`cvar value` lines only; a bare `cvar` line swallows the next line.)

## Stick vs mouse
In stick mode Steam maps the received gyro speed `r` linearly to stick deflection `(r - rmin)/(rmax - rmin)` (rmin ~3,
rmax ~43 deg/s measured). A game with its own inner dead zone `dz` then ignores small turns, so the daemon adds an
*anti-dead-zone*: once the speed exceeds a tiny gate, `antideadzone_dps` is added to it. Aim Lab fits the sensitivity and
anti-dead-zone from `dz` and the game's camera speed at full deflection (`aimlab/prefs.conf`). In mouse mode Steam's
response is linear (~98 counts per received degree) and the anti-dead-zone is forced to 0.

## Profiles
`motion/proxy.conf` holds global settings; `motion/profiles/<appid>.conf` (or `default.conf`) overlays them. The running
game is found by scanning `/proc/*/cmdline` for Steam's `reaper ... SteamLaunch ... AppId=<n>` every two seconds.
Keys: `mode` (stick|mouse), `enabled`, `sensitivity`, `invert_pitch/yaw/roll`, `compensation`, `smooth_threshold`,
`tighten_threshold`, `antideadzone_dps`, plus informational `name` and `gamepad_support`.

## Layouts
Steam layouts for non-Steam shortcuts and games are selected by name; the plugin writes
`.../Steam Controller Configs/<account>/config/<appid>/rp6 gyro <kind>_0.vdf` (header `url usercloud://<appid>/<name>`,
`export_type personal_local`) and selects it from the Steam UI with
`SteamClient.Input.SetSelectedConfigForApp(appid, 15, url, false, 2)`.
