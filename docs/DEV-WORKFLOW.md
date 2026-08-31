# Development Workflow — macOS dev machine + Linux hardware (fork)

Status: **proposed** · Date: 2026-08-31 · Scope: the `ui-rework` fork branch.

## 1. The problem

The fork owner develops at work on **macOS** (no Audient hardware), and the
target Linux machine with the iD24 lives at home. Goal: do all UI development
during the day on macOS, and verify against real hardware in the evening on
Linux — without writing throwaway code.

## 2. Why the current codebase cannot build on macOS

`lib/monix.cpp` includes ALSA headers (`<alsa/asoundlib.h>`) and talks ioctl to
`/dev/audient_idN` — Linux-only, and both are also hardware-dependent. The
frontend must therefore **never compile against `lib/monix` on macOS**.

## 3. Strategy: backend interface + mock device

```
┌────────────────────────── QML layer (the only UI code) ───────────────────┐
│  Fader.qml / Knob.qml / Meter.qml / RoutingMatrix.qml / MainWindow.qml …  │
└──────────────────────────────────────┬─────────────────────────────────────┘
                          mixerdevice (QObject adapter, talks only to an interface)
                                       │   normalized floats 0..1 in/out;
                                       │   write-immediate / read-polled
                            ┌──────────┴───────────┐
                            ▼                      ▼
                    DeviceBackend (abstract)      (CMake platform split)
                    ├─ RealBackend — Linux: wraps monix::Device (ioctl + ALSA)
                    └─ MockBackend — macOS / Linux / CI: pure-Qt device
                                     simulator; also selected by `--simulate`

```

Rules:

- **QML code is 100% shared** — the same files go into the macOS and Linux
  builds. Nothing UI-side is platform-specific.
- The `DeviceBackend` interface mirrors the `monix::Device` surface; the
  adapter and QML never know which backend is live.
- CMake: `if(APPLE)` (or `--simulate`) → `MockBackend`, `lib/monix` not
  compiled; on Linux by default → `RealBackend` + `lib/monix`. `monixctl`
  remains Linux-only.

### 3.1 MockBackend must emulate firmware semantics, not fake UI

A mock that just echoes values back would hide real bugs. It must reproduce
the device's actual behavioral contract (see ARCHITECTURE.md §5.1):

| Firmware behavior | Mock implementation |
|---|---|
| faders/phase/mixer cells are **write-only** (no GET) | store them; never return them — the persistence path (`state.conf`) gets genuinely exercised |
| monitor volume / master toggles / bus volumes / sample rate are readable | keep a "firmware state" map; GET returns the last SET value (matches real device) |
| meters | synthetic activity (per-strip noise/sine generators) delivered on the real 70 ms poll cadence |
| device disconnect/reconnect | a "unplug device" toggle → triggers the no-device UI and reconnect retry logic |
| clock-validity / crackle-warning scenario | a toggle to synthesize "optical clock present but source=Internal" so the warning path is testable |

- Expose the simulator as the `--simulate` runtime flag, not just an
  Apple-only build — so Linux machines without the interface can also run the
  GUI for demos and CI can smoke-test UI logic headlessly.
- Keep the mock's poll cadence (state 500 ms / meters 70 ms) identical to the
  real backend; the UI must not be able to tell which one it is.

## 4. Desktop-integration layer per platform

`desktopshell` (capability-detected, see UI-STACK.md §6) gains a macOS
implementation of the same capability interface:

| Feature | macOS | Linux (KDE/GNOME/Hyprland) |
|---|---|---|
| Tray | `QSystemTrayIcon` native | capability detection + degrade |
| Autostart | LaunchAgent (plist) | XDG autostart |
| Driver repair / elevation | **not applicable** → hide the feature entirely | `pkexec` → `sudo` fallback |
| Global shortcut | optional (Carbon/OSStatus) | xdg-desktop-portal, optional |

The settings UI is shared; only the backend capability flags differ.

## 5. Verification matrix — what gets checked where

| At work (macOS, mock backend) | At home (Linux, real backend) |
|---|---|
| All QML development: components, theme, layout, animations | Protocol verification: fader/routing/meter mapping on the iD24 |
| Persistence round-trip (write-only controls → `state.conf`) | ALSA behavior: speaker volume, clock source, optical validity |
| Drag-suppression, no-device UI, reconnect flow | Kernel module + `monixctl status` full chain |
| Settings UI against mock capabilities | PipeWire virtual devices, Linux tray, polkit repair flow |
| `qml` hot-reload for fast visual iteration | `git pull` → real build → verify → fix → push (notes for tomorrow) |

**Never verified on macOS** (no substitute for the evening session): the wire
protocol itself, ALSA behavior, kernel module, PipeWire split, Linux tray.

## 6. The daily loop

```
work, morning   git checkout ui-rework; qml --hot-reload …   (mock backend)
work, day       develop/iterate UI; every green state → git push fork ui-rework
work, evening   CI (macOS runner) confirms the push compiles on macOS
home, night     git pull; build with RealBackend; verify on the iD24;
                fix; push; note anything protocol-level for tomorrow
```

- CI: add a **macOS runner** job (build QML + MockBackend) alongside the
  existing Linux job, so a daytime push is compile-verified on both platforms
  before the evening session starts.
- Keep the fork private on GitHub if work policy prefers it; the workflow is
  identical either way.

## 7. Avoided approaches

- **Linux VM with USB passthrough on macOS**: QEMU audio-class USB
  passthrough on macOS is unreliable, and the hardware is at home anyway.
- **SSH-ing home from work for interactive dev**: latency and availability
  make it useless for UI iteration; the mock backend makes it unnecessary.
