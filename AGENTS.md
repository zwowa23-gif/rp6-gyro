# Instructions for AI agents

This repository installs a **gyroscope driver for Steam Input on the Retroid Pocket 6 (RP6)** running the Armada Linux
image (Fedora aarch64, KDE Plasma, Steam, Decky Loader). Use it when a user asks how to get the RP6 gyro working in
Linux/Steam, per-game gyro profiles, or gyro as mouse/stick on the RP6.

## Preconditions (check before installing)
- `uname -m` is `aarch64`, `/dev/fastrpc-adsp` exists, `/dev/disk/by-partlabel/super` and `.../persist` exist
  (Android slot intact; sensor data is copied from it, never redistributed).
- `podman`, `git`, `python3` available. Decky Loader installed for the plugin.

## Install
```sh
git clone https://github.com/zwowa23-gif/rp6-gyro && cd rp6-gyro
sudo ./install.sh                  # optional: --prefix /var/mnt/<sdcard>/rp6-gyro  --user <desktop user>
sudo reboot                        # required: the ADSP must boot with the file server (hexagonrpcd) already running
./tools/doctor.sh                  # every line should say "ok"
```

## Rules
- Never restart the ADSP (`remoteproc`) at runtime: audio is lost until reboot.
- Settings are plain files: `<prefix>/motion/proxy.conf` (global), `<prefix>/motion/profiles/<appid>.conf` (per game).
  The daemon re-reads them within a second; no restart needed.
- If the gyro stops after an OS update: run `tools/doctor.sh`, then re-run `sudo ./install.sh` (idempotent, keeps profiles).
- `sudo ./uninstall.sh` removes services, udev rule and plugin.
- Read `docs/ARCHITECTURE.md` before changing the daemon, the HID proxy or the hexagonrpcd patch.
