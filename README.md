# RP6 Gyro

Gyroscope for **Steam Input on the Retroid Pocket 6** running the Armada Linux image (Fedora, KDE Plasma, Steam,
Decky Loader). The RP6's IMU sits behind the Qualcomm ADSP and is invisible to the Linux kernel; this project makes it
readable and feeds it to Steam as the gyro of a Steam Deck controller, with **per-game profiles** managed from a Decky
plugin.

* gyro as a **gamepad stick** (games with controller support) or as a **mouse** (games without), chosen per game
* per-game sensitivity, axis inversion, drift compensation, optional smoothing, stick anti-dead-zone
* drift calibration from the plugin, and a full rotation/aim calibration in **RP6 Aim Lab**
* screen auto-rotation from the accelerometer

> Status: 1.0.0. Developed and tested on one device (kernel 7.2.6, Armada image, Steam ARM, Decky Loader). See
> [Known limitations](#known-limitations).

## Quick install

```sh
git clone https://github.com/zwowa23-gif/rp6-gyro && cd rp6-gyro && sudo ./install.sh && sudo reboot
```

Then open the Decky menu → **RP6 Gyro**. AI agents: see [AGENTS.md](AGENTS.md).

## How it works

```
QMI8658 --ADSP/SSC-- hexagonrpcd (file server for the sensors PD) -- libssc -- rp6imu (UDP 127.0.0.1:47811)
                                                                                    |
InputPlumber "Steam Deck" controller (hidraw 28de:12f0) --> rp6gyrod (uhid proxy, injects accel+gyro) --> Steam
                                                               |  per-game profile  (motion/profiles/<appid>.conf)
                                                               +- accelerometer uinput device -> KDE auto-rotate
```

* `hexagonrpcd` (patched) serves the sensor registry/DSP libraries from files extracted from your own Android
  partitions; the sensors firmware only starts if the file server is up while the ADSP boots, so a **reboot is
  required after installing**.
* `rp6gyrod` copies the controller report-for-report and fills in the IMU fields, so buttons, icons and back buttons
  stay exactly as InputPlumber provides them. It watches for the running game (`SteamLaunch ... AppId=`) and applies
  that game's profile on top of the global settings.
* The Decky plugin edits profiles, generates Steam layouts from Steam's own templates and starts calibration.

More detail in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Requirements

* Retroid Pocket 6 with the Armada image, `aarch64`, kernel with `/dev/fastrpc-adsp`
* the **Android slot kept intact** (`super` and `persist` partitions): the sensor registry and DSP libraries are
  copied from there at install time. They are not redistributed here
* `podman`, `git`, `python3`; Decky Loader for the plugin; internet access for the build

## Install

```sh
git clone https://github.com/zwowa23-gif/rp6-gyro && cd rp6-gyro
sudo ./install.sh                       # or: sudo ./install.sh --prefix /var/mnt/<sdcard>/rp6-gyro
sudo reboot
./tools/doctor.sh                       # after the reboot
```

The installer builds everything in podman containers (pinned upstream revisions, see `UPSTREAM`), extracts the Android
data, installs three services (`rp6-hexagonrpcd`, `rp6-deckproxy`, user service `rp6-motion`), the udev rule and the
Decky plugin. It is idempotent; run it again to update. `sudo ./uninstall.sh` removes it.

For the full calibration add `<prefix>/out/rp6aim` to Steam as a non-Steam game named exactly **RP6 Aim Lab**.

## Using it

Open the Decky quick-access menu and choose **RP6 Gyro**.

* **Game profile** follows the running game; pick another entry to edit it, or *Default* for games without a profile.
  A profile is created the first time a game runs: *gamepad stick* for games with full controller support, *mouse*
  for everything else.
* **Gyro drives** selects stick or mouse. Settings that only apply to one mode are hidden in the other.
* **Apply matching layout** writes a Steam layout for that game (generated from Steam's stock templates) and selects
  it: stick → gamepad layout with gyro on the right stick; mouse → gamepad layout with gyro on the mouse, or keyboard
  + mouse for games without gamepad support. It replaces the layout currently selected for that game.
* **Calibrate drift**: put the console on a table for two seconds. **Full calibration** runs the rotation test in Aim
  Lab and writes the fitted sensitivity (and anti-dead-zone in stick mode) to the profile.

Settings are plain files: `<prefix>/motion/proxy.conf` (global) and `<prefix>/motion/profiles/*.conf`.

## Known limitations

* RP6 only; it relies on the RP6's Android partition layout and sensor setup.
* After an image update that changes InputPlumber's controller target, the kernel or the ADSP firmware, the gyro may
  stop working while everything else keeps working (the original controller is shown again whenever the proxy is
  down). Run `tools/doctor.sh`; re-run `install.sh` if files were reset.
* The ADSP must not be restarted at runtime (audio is lost until reboot).
* Steam's controller-support level is a store tag; some games tagged *partial* work poorly with a gamepad, so they get
  the mouse default. Change the mode per game if that is wrong.
* The generated keyboard + mouse layout uses Steam's default WASD template with an eight-way left stick; adjust it in
  Steam if a game needs different keys.

## Credits and license

Built on [hexagonrpc](https://github.com/linux-msm/hexagonrpc) (patched: write support for the sensor registry) and
[libssc](https://codeberg.org/DylanVanAssche/libssc) (patched: sample rate selection), both GPL-3.0. The patches are in
`patches/`. This project is licensed **GPL-3.0-or-later** (see `LICENSE`), because `rp6imu` links libssc.
