# CLAUDE.md

Agent guide for this repository. This file is the canonical source for both
Claude Code and pi — `AGENTS.md` only points here, so **edit this file, not that
one**.

## Project

Monix is a Linux control app + companion kernel driver for Audient iD-series USB
audio interfaces (VID `0x2708`). It exists because the iD control protocol goes
over a USB interface owned by `snd-usb-audio`, which userspace can't send class
requests to without tearing down the sound card — so a kernel module
(`driver/audient_id.c`) relays control transfers from inside the kernel while
audio keeps playing. Protocol semantics were reverse-engineered from the
official macOS/Windows apps. **Only the iD24 is hardware-verified**; other
models are RE'd but untested.

Key docs: `docs/PROTOCOL.md` (wire protocol spec), `docs/DEVICES.md` (per-model
RE). Planning docs for the Qt/QML fork UI (`ARCHITECTURE.md`, `UI-STACK.md`,
`DEV-WORKFLOW.md`) live under `monix-qt-ui/docs/` on the `feat/qt-qml-ui`
branch — they are not on `master` yet.

## Repository status (updated 2026-11-02)

This repository is a **fork of `sKuhLight/monix`** that is effectively
self-maintained:

- Upstream has been inactive since 2026-07-07 (no commits, one unanswered
  install bug, no PR history, no branch protection).
- `upstream` remote is configured and points at `sKuhLight/monix`; fetch it to
  check whether upstream resumed.
- **`master` here is our integration trunk** — it no longer mirrors
  `upstream/master`, and that is intentional. Users clone this repo, so trunk
  carries our fixes and features.
- Patches that are useful upstream are still offered as PRs from short-lived
  branches (PRs #2 and #3 are open as of the date above).

### Branch state

| Branch | State |
|---|---|
| `master` | trunk: `chore: ignore .env` + `fix/dkms-toolchain` + `fix/window-app-id` + `feat/optical-clock-persist` |
| `feat/optical-clock-persist` | merged into trunk, kept for history / can be deleted |
| `fix/dkms-toolchain` | merged into trunk; also open upstream as PR #2 |
| `fix/window-app-id` | merged into trunk; also open upstream as PR #3 |
| `feat/qt-qml-ui` | **not merged** — Qt 6 Quick/QML scaffold + planning docs; UI direction still undecided |
| `fix/suspend-resume-uaf` | 3 commits on top of trunk: bug report + `kref` lifetime fix in the driver + stale-fd recovery in the GUI; **not yet verified on hardware**. Merges into trunk once a real suspend/resume passes |

### Work queue

1. `fix/suspend-resume-uaf`: kernel use-after-free on suspend/resume, reported
   in `docs/BUG-suspend-resume-use-after-free.md`. Commits done: (a) the report,
   (b) `kref` + `.release` in `driver/audient_id.c` so `struct idctl_dev`
   outlives open fds, (c) drop the stale fd on `-ENODEV`/`-EBADF`/`-ESHUTDOWN`
   in `lib/monix.cpp` so the existing reconnect path in `app/main.cpp` re-opens
   the device. **Remaining: hardware verification** (`pm_test` reproducer
   below), then `git merge --no-ff` into `master` and push; the driver half is
   also worth offering upstream since it is a kernel memory-safety bug.
2. Upstream issue #1 (Ubuntu 26.04 install failure) — the main cause is fixed by
   PR #2; the remaining two root causes are unowned: `/etc/os-release` clobbering
   the module version in `tools/monix-driverctl` (`install_deps()` sources it into
   the same shell that holds `VERSION`), and kbuild rejecting build paths that
   contain spaces.
3. Decide whether `feat/qt-qml-ui` becomes the future UI or gets abandoned; if it
   lands, define the retirement point for the ImGui UI.

## Branch & commit conventions

- **Topic branches** are `feat/`, `fix/`, `docs/`, `chore/` + a short
  kebab-case subject: `fix/dkms-toolchain`, `feat/optical-clock-persist`. One
  concern per branch — do not mix a feature with drive-by fixes.
- **Daily work branches from `master`.** For a patch meant for upstream, branch
  from `upstream/master` instead and cherry-pick what is needed, so the PR diff
  stays free of fork-only commits.
- **Commit messages**: `<scope>: <imperative summary>` in the subject, 72 cols.
  Scopes in use: `gui:`, `driver:`, `lib:`, `install:`, `docs:`, `chore:`.
  Bodies explain *why* (failing command, observed symptom) and name the
  environment that was verified (distro, kernel, device). See
  `git log 121a1f5..master` for examples.
- **Merging into `master`**: use `git merge --no-ff`. This keeps the original
  commit SHAs, so branches that are also open as upstream PRs do not need a
  force-push afterwards.
- **Never rewrite `master`** (no rebase/force-push); fix forward instead.
- Keep fork-only work and generally useful fixes in separate branches.

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
via `monixctl` (subcommands: `status`, `monitor V`,
`master <mono|phase|cut|dim|talk|alt> on/off`, `sr HZ`, `mixer CELL V`,
`hp N V`, `meters`, `route OUT CODE`, `restore-main`).

Before installing a freshly built module over the loaded one on a dev machine,
take a filesystem snapshot (`snapper -c root create -d "before audient_id ..."`
on btrfs) — a bad module can wedge the USB device or the boot path.

CI (`.github/workflows/ci.yml`) compiles userspace (Ubuntu) and the kernel
module. Two things to know about it:

- The kernel job builds with `make -C driver`, i.e. it **does not exercise
  `driver/dkms.conf`**; DKMS-specific breakage (see upstream issue #1) slips
  through.
- It only triggers on push to `master` and on pull requests, so work on other
  branches and squash-merged PRs are only covered once they land on trunk.

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
- **`struct idctl_dev` outlives its device node**: `misc_deregister()` only
  unlinks `/dev/audient_idN`, it does not drain open fds, so the driver holds a
  `kref` (probe + one per `open()`) and frees only when the last fd closes —
  never `kfree()` from `idctl_disconnect()`. After disconnect `dev->udev` is
  NULL and every transfer returns `-ENODEV`; `lib/monix` closes the fd on that
  error and `app/main.cpp`'s 5 s check reopens the node. Keep all `dev->udev`
  reads (including error logging) inside `dev->lock`.
  The `kref_get()` in `idctl_open()` needs no extra locking because the misc
  core calls `->open()` with `misc_mtx` held and `misc_deregister()` takes the
  same lock — verified by disassembling `misc_open()`/`misc_deregister()` in
  `/usr/src/linux/vmlinux` on 7.2.11-arch1-1. If a future kernel calls `->open()`
  outside that lock again, the open path needs its own registry/lock.
- **Suspend/resume drops the device node**: every USB suspend/re-enumeration
  runs `idctl_disconnect()`, so any long-lived fd is stale after resume. The
  reproducer without a real sleep is `echo devices | sudo tee
  /sys/power/pm_test`, `echo mem | sudo tee /sys/power/state`, then `echo none`
  back; watch that `monix-gui` survives, `dmesg` has no GPF, and the mixer
  reconnects on its own.
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
- **Firmware forgets optical-in mode and clock source across power cycles** —
  `app/appsettings.h` persists them to `~/.config/monix/app.conf` and
  `app/main.cpp` re-applies them on every connect (optical-in defaults to
  S/PDIF, clock auto-slaves to a valid optical clock).
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
- **Window identity matters**: `app/main.cpp` sets `GLFW_WAYLAND_APP_ID` /
  `GLFW_X11_CLASS_NAME` to `monix` (GLFW ≥ 3.4) and the desktop entries carry
  `StartupWMClass=monix`, otherwise shells can't match the window to the icon.

## Constraints

- Kernel module code must stay GPL-2.0 compatible; it only forwards class
  requests (`wLength ≤ 64`) — do not turn it into a raw-USB backdoor.
- The module must match the running kernel (Clang vs GCC builds are
  auto-detected by `driver/Makefile`); after kernel updates the GUI offers a
  rebuild via `monix-driverctl install-current`. Never hardcode `LLVM=1` in
  `dkms.conf`.
- kbuild does not support build directories containing spaces; `make -C driver`
  from such a path fails with a confusing "external module directory ... does
  not exist". Use a space-free path (DKMS builds under `/var/lib/dkms`, so it is
  unaffected).
- The library is never installed — it's static-linked into `monix-gui` and
  `monixctl` (no `install()` rules exist).
- Version `0.1` is duplicated in `CMakeLists.txt`, `driver/dkms.conf` and the
  `install.sh` fallback; the release workflow additionally writes a `VERSION`
  file. Change all of them together until this is centralised.
- `LICENSE` is MIT (© sKuhLight). Fork-only additions are covered by the same
  file; keep the original copyright notice.

## Where to look

- **GUI**: `app/` (`main.cpp` is the whole ImGui frontend; `tray.cpp`,
  `appsettings.h`, `persist.h`, `drivercheck.h`, `pwsplit.h`)
- **Protocol/library**: `lib/monix.h`, `lib/monix.cpp`
- **Kernel module**: `driver/audient_id.c`, `driver/dkms.conf`,
  `driver/99-audient-id.rules`
- **CLI**: `tools/monixctl.cpp`; **driver maintenance**: `tools/monix-driverctl`
- **Docs**: `docs/PROTOCOL.md`, `docs/DEVICES.md`, `README.md`
- **Installer/release**: `install.sh`, `.github/workflows/{ci,release}.yml`
