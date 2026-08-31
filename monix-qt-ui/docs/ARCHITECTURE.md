# Monix — Architecture Overview

This document is a bird's-eye summary of how Monix is put together: the layer
split, the communication contracts between layers, the interaction patterns,
and what a reimplementation of any layer must know. For wire-level protocol
details see [PROTOCOL.md](../../docs/PROTOCOL.md); for per-device profiles see
[DEVICES.md](../../docs/DEVICES.md). Both live in the repository's upstream
`docs/` tree (the external doc library, kept in sync with upstream).

Unless another location is given, paths in this document are repo-
root-relative: `app/`, `lib/`, `driver/`, `tools/`, `setup/` are upstream
files; `monix-qt-ui/` is this fork's UI project. Fork references to the
upstream doc library are written `../../docs/…`.

## 1. What Monix is

A Linux control application **and** companion kernel driver for Audient iD-series
USB audio interfaces. The core problem it solves:

- The iD control protocol is sent as USB class control requests to the
  AudioControl interface (interface 0), which on Linux is owned by
  `snd-usb-audio`. Userspace cannot send such requests without detaching that
  driver, which tears down the whole sound card ("no audio while the mixer app
  is open").
- The companion module (`audient_id`) performs the transfers **from inside the
  kernel** instead, where the restriction does not exist. `snd-usb-audio` keeps
  interface 0 the entire time, so control and audio run simultaneously.

Only the iD24 is hardware-verified; other models are reverse-engineered from the
official macOS/Windows apps (see ../../docs/DEVICES.md).

## 2. Layer diagram

```
┌──────────────────────────────────────────────────────────────┐
│ Frontends (both consumers of lib/monix, never talk ioctl)     │
│                                                               │
│   monix-gui        monixctl         (your fork's new frontend)│
│   app/main.cpp     tools/monixctl.cpp                         │
└──────────────┬────────────────────────────────────────────────┘
               │  C++ calls on monix::Device (~30 methods)
               ▼
┌──────────────────────────────────────────────────────────────┐
│ lib/monix  — protocol semantics (static lib, build-internal) │
│   encoding, entity/selector map, float↔int16 scaling,        │
│   ALSA wrappers, per-device profiles                         │
└───────┬──────────────────────────────────┬───────────────────┘
        │  ioctl on /dev/audient_idN       │  ALSA API (userspace)
        ▼                                  ▼
┌──────────────────────────────┐   ┌──────────────────────────┐
│ audient_id kernel module     │   │ snd-usb-audio controls   │
│ (thin validated relay:       │   │ "Speaker Playback Volume",│
│  USB transport only)         │   │ clock source, optical    │
│  write() = OUT,              │   │ clock validity           │
│  IDCTL_IOC_XFER ioctl = bidir│   └──────────────────────────┘
└───────┬──────────────────────┘
        │ usb_control_msg_send/recv on ep0
        ▼
   iD firmware (class requests)
```

Notes:

- **The "backend" is lib/monix + the kernel module.** The module is a stateless
  byte relay — it knows no mixer semantics; all protocol meaning lives in the
  library.
- **lib/monix is the only place in the codebase that calls `ioctl`**
  (`lib/monix.cpp:119`). Both frontends reach the hardware exclusively through
  `monix::Device` methods.
- **Not everything goes through the module.** The *audible* monitor volume
  (DAW→output level), clock source, and optical clock validity live in
  `snd-usb-audio` and are reached via ALSA from inside the library
  (`lib/monix.cpp:160-248`).
- The GUI additionally shells out to external commands (see §6).

## 3. Component map

| Path | Role | Key facts |
|---|---|---|
| `driver/audient_id.c` | Kernel module (GPL-2.0, ~275 LOC) | Binds the otherwise-unused **DFU interface** (`APP_SPEC 0xFE` / subclass `0x01`) of any device with VID `0x2708`; misc char device `/dev/audient_idN` (mode 0660, group via udev rule); IDA numbering; mutex-serialized transfers; `udev=NULL` on unplug → `-ENODEV`; **only forwards class requests** (not a raw-USB backdoor); uses `usb_control_msg_send/recv` (kernel ≥ 5.9) |
| `lib/monix.h/.cpp` | Protocol library (static lib `monix`, built by root CMake) | `Device` class: monitor section, mixer matrix, routing, headphones, input phase/gain, sample rate, optical ADAT/S-PDIF mode, meters; ALSA wrappers; per-device profiles (9 models, PID fallback → iD24) |
| `app/` | GUI frontend | Dear ImGui 1.92.4 (fetched via FetchContent) + GLFW + OpenGL 3.0; `main.cpp` (~720 LOC) is the whole UI; small header-only helpers: `theme`, `persist`, `pwsplit`, `drivercheck`, `appsettings`, `tray` (AppIndicator, optional) |
| `tools/monixctl.cpp` | CLI frontend | One-shot commands: status / monitor / master / sr / mixer / hp / meters / route / restore-main. Purpose: verify the library against hardware |
| `tools/monix-driverctl` | Driver maintenance (bash) | `status` (key=value diagnostics), `install-current` (DKMS rebuild via pkexec/sudo re-exec), `install-deps` (per-distro) |
| `capture/` | RE workflow | Windows VM + usbmon capture of the official app, `decode.py` → authoritative control map |
| `re/` | Static RE working files | macOS `iD.app` slice disassembly (gitignored) |
| `setup/` | PipeWire split configs | Hand-editable equivalent of the GUI's Virtual Devices |
| `docs/` (upstream) | PROTOCOL.md, DEVICES.md — the external doc library; stays in sync with upstream, do not divert in fork commits |
| `monix-qt-ui/docs/` | fork planning docs: this file, UI-STACK.md, DEV-WORKFLOW.md | |

## 4. Communication contracts

### 4.1 The userspace↔module ABI: `struct idctl_msg`

Defined identically on both sides (byte-for-byte, packed):
`driver/audient_id.c:53` and `lib/monix.cpp:18`.

```c
struct idctl_msg {          /* 16-byte header + 64-byte data */
    __u8  bRequestType;     /* 0x21 = SET, 0xA1 = GET (0x80 bit = IN) */
    __u8  bRequest;         /* 0x01 = SET_CUR/GET_CUR, 0x03 = GET_MEM */
    __u16 wValue;           /* (selector<<8) | channel */
    __u16 wIndex;           /* (entity<<8) | 0 */
    __u16 wLength;          /* <= 64 */
    __u8  data[64];
};
```

- `write()` → OUT-only transfer (kernel rejects the IN bit with `-EPERM`).
- `IDCTL_IOC_XFER` (`_IOWR(0xA1, 1, struct idctl_msg)`) → bidirectional; for IN
  requests the response is copied back into the same struct's `data[]`.
- One ioctl == exactly one USB control transfer. Synchronous and blocking.
- Module-side validation: class requests only (`bmRequestType & 0x60 == 0x20`),
  `wLength <= 64`, serialized by a mutex, device-presence checked.

### 4.2 USB wire encoding (from ../../docs/PROTOCOL.md)

```
wValue = (selector<<8) | channel      wIndex = (entity<<8)
SET = bmRequestType 0x21, bRequest 0x01      GET = 0xA1 / 0x01
Meters: GET_MEM 0xA1/0x03, wValue = offset   (one transfer = one whole block)
```

Key entities (iD24, from device descriptors): `0x36` monitor, `0x3c`
mixer/meters, `0x33` routing, `0x0b` inputs, `0x0c` headphones/mix-bus masters,
`0x29` sample rate, `0x01`/`0x14` digital optical mode.

### 4.3 Value encoding

- Volumes: `int16` in 1/256 dB — `0x0000` = 0 dB (max), `0x8000` = mute.
  Library maps `float 0..1 ↔ [-60 dB .. 0 dB]` (`floatToVol`/`volToFloat`,
  `lib/monix.cpp:79-92`), round-trippable.
- Meters: raw uint16 LE per channel / 65535, block layout in node order.
- Mixer matrix addressing: `cell = input*6 + bus*2 + chan` — bus 0 = Main,
  1 = Cue A, 2 = Cue B (16 inputs × 6 sends = 96 cells). A stereo strip writes
  all four crosspoints (L→busL, R→busR, cross-feeds set to 0).

## 5. Interaction patterns

Single-threaded event loop. **Writes are immediate, reads are polled.**

| Direction | Rhythm | Where |
|---|---|---|
| Control writes (fader/routing/toggles) | Sent the moment the user acts; no batching/debounce | `app/main.cpp` |
| State reads (sample rate, monitor vol, 6 master toggles, 3 bus vols) | Every **500 ms** | `main.cpp:327` |
| Meters (16 in + 6 out) | Every **70 ms**, one GET_MEM block read each | `main.cpp:333` |
| Digital mode / clock / optical validity (ALSA) | Every 1 s | `main.cpp:573,596` |
| Driver health (`monix-driverctl status`) | Every 5 s | `main.cpp:308` |
| State persistence snapshot | Every 800 ms | `main.cpp:712` |

- **Drag suppression**: while a fader/knob is being dragged (`mainActive`,
  `busActive`), the poll read-back is suspended — otherwise the stale polled
  value snaps the control back (`main.cpp:329,454`).
- **Meter polling must stay block-based** (one transfer per 16/12 bytes) — per-
  channel reads flood ep0 and the kernel resets the device (../../docs/PROTOCOL.md §5).

### 5.1 Write-only controls (firmware limitation)

The iD firmware does **not** report back faders/phase/mixer cells (GET_CUR
stalls; see ../../docs/PROTOCOL.md §3). Only monitor volume, master toggles, bus volumes,
sample rate and meters are readable. Consequences:

- The GUI keeps its own local mirror of all write-only controls and persists it
  every 800 ms to `~/.config/monix/state.conf` (format: lines `faders ...`,
  `phase ...`, `links ...`, `phones ...` — see `app/persist.h`), restoring it at
  launch. **Any new frontend must do the same** — it cannot read these back.

## 6. GUI → system command paths (bypass lib)

| Purpose | Command | Mechanism |
|---|---|---|
| Driver health / repair | `monix-driverctl status` / `install-current` | `popen` (drivercheck.h); repair spawned detached, log at `/tmp/monix-driverctl.log`; elevated via pkexec/sudo re-exec inside the script |
| Virtual device split | `pw-loopback --capture-props ... --playback-props ...` | double-fork + setsid so devices survive GUI exit; PID files in `~/.cache/monix/`; enabled-state detection via `pactl list short sinks/sources`; persistent config written to `~/.config/pipewire/pipewire.conf.d/id24-split.conf` (pwsplit.h) |

## 7. Build & install shape

- Root CMake builds `monix` (STATIC) + `monixctl`; `app/` is added only if it
  exists (`CMakeLists.txt:19-21`).
- **The library is never installed** — there are no `install()` rules. It is an
  intermediate artifact (`build/libmonix.a`) statically linked into both
  binaries. Release bundles ship only prebuilt `monix-gui`/`monixctl` plus the
  driver source (modules must be built against the running kernel).
- `install.sh` installs: `/usr/local/bin/{monix-gui,monixctl,monix-driverctl}`,
  driver source under `/usr/share/monix/driver` (for later repairs), DKMS
  registration, udev rule, desktop launcher; adds user to `audio` group.
- External deps of lib: ALSA only. Of GUI: GLFW, OpenGL, ImGui (fetched),
  optional AppIndicator (`ayatana-appindicator3-0.1`, fallback
  `appindicator3-0.1`).

## 8. Frontend replaceability (fork notes)

The GUI is a leaf component with no influence on the rest of the system:

- Replace the frontend and lib/driver are untouched. The `Device` API (~30
  methods) is the entire surface, plus `monixctl` as a working reference for
  exercising every call against hardware.
- A new frontend must replicate: the `state.conf` persistence format, the
  poll cadences (or its own — the protocol has no rate constraints beyond the
  no-per-channel-metering rule), the external-command integrations it wants
  (drivercheck, pwsplit, tray), and the ALSA-side volume handling for the
  audible monitor level.
- The protocol itself is small and fully specified in ../../docs/PROTOCOL.md — a
  from-scratch implementation in another language is feasible without touching
  the kernel module (the ioctl ABI above is the only contract).

## 9. Current status & known gaps

- **Hardware-verified: iD24 only.** iD48 is RE'd but untested (its routing
  constants differ markedly — do not reuse iD24 codes).
- **Table-scheme devices (iD14/22/44): routing is not wired up** — the
  `getRouterSourceIndex` formula is undecoded; GUI shows routing read-only.
- iD4: no mixer/routing; only input gain/phase and monitor controls.
- Some controls are write-only in firmware (see §5.1) — state must be
  client-maintained.
- No automated tests; hardware verification is the only check.
- Entity IDs are per-USB-descriptor — non-iD24 profiles assume iD24 entity IDs
  until confirmed on hardware (../../docs/DEVICES.md "Open items").
