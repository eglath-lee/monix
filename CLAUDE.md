# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Monix is a Linux control app + companion kernel driver for Audient iD-series USB
audio interfaces (VID `0x2708`). It exists because the iD control protocol goes
over a USB interface owned by `snd-usb-audio`, which userspace can't send class
requests to without tearing down the sound card — so a kernel module
(`driver/audient_id.c`) relays control transfers from inside the kernel while
audio keeps playing. Protocol semantics were reverse-engineered from the
official macOS/Windows apps. **Only the iD24 is hardware-verified**; other
models are RE'd but untested.

Key docs: `docs/ARCHITECTURE.md` (system overview — read first),
`docs/PROTOCOL.md` (wire protocol spec), `docs/DEVICES.md` (per-model RE).

## Build & dev commands

Requires: C++17, CMake ≥ 3.16, kernel headers, ALSA/GLFW/OpenGL dev packages
(see README "Requirements" for per-distro lists). First GUI build fetches
Dear ImGui from the network.

```sh
# Userspace: library + CLI + GUI
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/app/monix-gui     # GUI (needs module loaded + device attached)
./build/monixctl status   # CLI: live device state dump

# Kernel module (must match the running kernel)
make -C driver            # builds driver/audient_id.ko
# DKMS (recommended, survives kernel updates):
sudo cp -r driver /usr/src/audient_id-0.1 && sudo dkms add -m audient_id -v 0.1 \
  && sudo dkms install -m audient_id -v 0.1
sudo cp driver/99-audient-id.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules
sudo modprobe audient_id  # or replug; creates /dev/audient_idN (audio group)

# Full installer (deps + driver + app + helper)
./install.sh
```

There is **no automated test suite** — verification is against real hardware
via `monixctl` (subcommands: `status`, `monitor V`, `master <mono|phase|cut|dim|talk|alt> on/off`,
`sr HZ`, `mixer CELL V`, `hp N V`, `meters`, `route OUT CODE`, `restore-main`).
CI (`.github/workflows/ci.yml`) only compiles both userspace and the module.

## Architecture

Layered, with a hard frontend/backend split:

```
frontends (GUI app/, CLI tools/monixctl.cpp)      — call monix::Device methods only
        │
lib/monix.h/.cpp — protocol semantics (static lib `monix`; encoding, scaling,
        │          ALSA wrappers, per-device profiles)   ← ONLY place that does ioctl (lib/monix.cpp:119)
        ├── ioctl IDCTL_IOC_XFER on /dev/audient_idN → kernel module (USB relay) → device ep0
        └── ALSA userspace API  → snd-usb-audio controls (audible monitor volume,
                                  clock source, optical clock validity)
```

Facts that matter when editing:

- **The kernel module is a stateless relay** — no mixer semantics; it only
  validates class requests and forwards them. All protocol meaning lives in
  `lib/monix`.
- **Userspace↔module ABI**: `struct idctl_msg` (16-byte header + 64-byte data)
  is defined packed and byte-identical on both sides (`driver/audient_id.c:53`
  and `lib/monix.cpp:18`). One ioctl/write = one USB control transfer, sync and
  blocking. `write()` is OUT-only; the ioctl is bidirectional (IN responses
  copy back into the same struct).
- **Wire encoding**: `wValue=(selector<<8)|channel`, `wIndex=(entity<<8)`;
  SET = bmRequestType `0x21`/bRequest `0x01`, GET = `0xA1`/`0x01`; meters use
  GET_MEM `0xA1`/`0x03` with the offset in wValue. Key entities (iD24): `0x36`
  monitor, `0x3c` mixer/meters, `0x33` routing, `0x0b` inputs, `0x0c`
  headphones/bus masters, `0x29` sample rate.
- **Volumes are int16 in 1/256 dB** (`0x0000`=0 dB, `0x8000`=mute); the library
  maps `float 0..1 ↔ -60..0 dB` (`floatToVol`/`volToFloat`). Mixer cells:
  `cell = input*6 + bus*2 + chan` (bus 0/1/2 = Main/Cue A/Cue B).
- **Interaction pattern**: writes are immediate, reads are polled
  (state ~500 ms, meters ~70 ms, ALSA controls ~1 s, driver health ~5 s). While
  a control is being dragged, its read-back poll is suppressed or the stale
  value snaps the control back.
- **Firmware is write-only for faders/phase/mixer cells** (GET_CUR stalls) —
  the GUI maintains its own mirror and persists it to
  `~/.config/monix/state.conf` (`app/persist.h`; format: `faders`/`phase`/
  `links`/`phones` lines). Any new frontend must replicate this.
- **Meters must be read as whole blocks** (one GET_MEM per 16/12-byte block),
  never per-channel — that floods ep0 and the kernel resets the device.
- **GUI shells out** (bypassing lib): `monix-driverctl status/install-current`
  for kernel-update detection & DKMS repair (`app/drivercheck.h`), and
  `pw-loopback`/`pactl` for the Windows-style virtual-device split
  (`app/pwsplit.h`, PID files in `~/.cache/monix/`).
- **Profile-driven**: `DeviceInfo` table at `lib/monix.cpp:45` (9 models).
  Unknown PID falls back to the iD24 profile. Table-scheme devices
  (iD14/22/44) have routing **not wired up**; iD48 routing constants differ
  from iD24 — never reuse iD24 codes for it (see DEVICES.md).
- **Entity IDs are per-USB-descriptor**; non-iD24 profiles assume iD24 entity
  IDs until confirmed on hardware.

## Constraints

- Kernel module code must stay GPL-2.0 compatible; it only forwards class
  requests (`wLength ≤ 64`) — do not turn it into a raw-USB backdoor.
- The module must match the running kernel (Clang vs GCC builds are
  auto-detected by the Makefile); after kernel updates the GUI offers a
  rebuild via `monix-driverctl install-current`.
- The library is never installed — it's static-linked into `monix-gui` and
  `monixctl` (no `install()` rules exist).
