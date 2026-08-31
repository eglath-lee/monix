# UI Tech-Stack Decision (fork planning)

Status: **proposed** · Date: 2026-08-31 · Scope: the Monix fork's new frontend only.

## 1. Context

The current GUI (`app/`, Dear ImGui 1.92.4 + GLFW + OpenGL 3.0) is a single
721-line `main.cpp` with hand-drawn controls (custom-drawn knobs/meters via
`ImDrawList`, stock sliders for faders) and a 55-line color palette. On Wayland
desktops it runs through XWayland, and the immediate-mode visual ceiling is the
reason the default look is considered poor.

The fork plan is to **reimplement the frontend only**. The backend is stable
and must remain untouched:

- `driver/audient_id.c` — kernel module (USB relay; GPL-2.0)
- `lib/monix.h/.cpp` — protocol library (static lib, the only ioctl caller)
- `tools/monixctl.cpp` — CLI verifier (the correctness reference for every
  device call)
- `tools/monix-driverctl`, `capture/`, `setup/`, `docs/`

See `docs/ARCHITECTURE.md` for the layering; the frontend contract is just the
`monix::Device` API (~30 methods) plus the persistence format.

## 2. Requirements

Functional constraints inherited from the protocol (non-negotiable):

- **Write-only controls**: firmware never reports back faders/phase/mixer
  cells — the frontend must keep its own mirror and persist it
  (`state.conf`, see ARCHITECTURE.md §5.1).
- **Meter reads must be whole-block** (one GET_MEM per 16/12-byte block at
  ~70 ms); per-channel reads flood ep0 and reset the device.
- **Write-immediate / read-polled** interaction; drag-suppression on controls
  being dragged.

Frontend requirements for this fork:

| # | Requirement | Note |
|---|---|---|
| R1 | Native Wayland, good HiDPI | target desktop is Hyprland; must not rely on XWayland |
| R2 | Custom-drawn mixer controls (fader/knob/meter) with a proper visual design | the whole point of the rewrite |
| R3 | Works on KDE Plasma and GNOME, not just Hyprland | see §6 |
| R4 | Zero changes to `lib/` and `driver/` | adapter layer must be thin |
| R5 | License compatible with an MIT fork | no GPL/commercial traps |
| R6 | Reuse the external-command integrations | `monix-driverctl` (repair), `pw-loopback`/`pactl` (virtual devices) |
| R7 | `monixctl` stays as the verification reference during development | |

## 3. Candidates

### A. Qt 6 Quick / QML (recommended)

Standalone QML app (`QQmlApplicationEngine` host + QML UI), C++ adapter over
`monix::Device`.

- Native Wayland + HiDPI out of the box; also runs on X11/KDE/GNOME unchanged.
- QML is declarative and purpose-built for custom control panels — fader
  tracks, knob dials, gradient meters are declarative components, not raw
  draw calls.
- C++ ↔ QML bridge is first-class: wrap `Device` in a `QObject`, expose
  properties/signals; protocol stays C++.
- LGPL — clean for an MIT fork.
- Fits the user's desktop stack: Quickshell is itself Qt Quick/QML, so one
  technology spans the whole desktop (a shell bar widget can later reuse the
  same QML components).
- Known cost: Qt is a large dependency; QML apps on GNOME desktops need
  `qt6-declarative` (+ `qt6-wayland`).

### B. Keep ImGui, restyle

Cheapest option: new fonts, proper fader/knob/meter drawing, component split.
- Stays on XWayland (GLFW) on Wayland; HiDPI mediocre.
- Immediate-mode ceiling remains; no CSD/titlebar polish; system integration
  (tray etc.) still hand-rolled.
- Viable only if the goal is "less ugly" rather than "native-quality UI".

### C. GTK4 / libadwaita

- Native Wayland, but custom audio controls require GSK canvas drawing — much
  more laborious than QML for fader/knob/meter work.
- libadwaita's design language fights a dense mixer layout; tray is still a
  per-desktop problem.
- No synergy with the user's Quickshell/QML desktop.

### D. Slint (C++ or Rust)

- Designed exactly for control-panel UIs; good looking out of the box.
- License is GPL or commercial — conflicts with the MIT fork's clean
  licensing story (R2/R5).
- Smaller ecosystem; binary font/asset handling is fine but less familiar.
- Only worth it if the user rejects Qt on principle.

### E. Rust frontends (egui / iced)

- egui is immediate-mode like ImGui — same visual ceiling as today, plus the
  cost of a Rust rewrite of the protocol layer (or FFI to the C++ lib).
- iced is retained-mode with a nicer look, but the ecosystem and custom
  drawing maturity are still behind QML for dense audio UI.
- Only if the fork owner is primarily a Rust developer and accepts the
  protocol-port cost.

### F. Web tech (Tauri / Electron)

- CSS makes beautiful meters/faders trivial, and a sidecar can bridge to
  `monixctl`/the lib.
- Heavyweight for an audio utility on a lean Hyprland setup; tray/autostart
  support is a patchwork.
- Not recommended for this project's profile.

## 4. Comparison

| Criterion | A: Qt QML | B: ImGui restyle | C: GTK4 | D: Slint | E: Rust | F: Web |
|---|---|---|---|---|---|---|
| Native Wayland / HiDPI | ✅ | ❌ (XWayland) | ✅ | ✅ | ✅ | ~ |
| Custom control suitability | ✅✅ | ~ | ❌ | ✅ | ~ | ✅ |
| Backend binding cost | low (C++ direct) | none | medium | low | high (port/FFI) | medium (sidecar) |
| Desktop integration (tray/polkit) | ✅ (Qt + XDG) | ~ | ✅ | ~ | ~ | ~ |
| License fit (MIT fork) | ✅ LGPL | ✅ MIT | ✅ LGPL | ❌ GPL/commercial | ✅ | ✅ |
| Aesthetic ceiling | high | low | medium | high | medium | high |
| Fits Hyprland + Quickshell stack | ✅✅ | ❌ | ❌ | ~ | ~ | ❌ |
| Ecosystem stability | ✅ | ✅ | ✅ | medium | medium | ✅ |

## 5. Recommendation

**Qt 6 Quick / QML standalone app (Option A).** Rationale in one line: it is
the only option that is native-Wayland, purpose-built for custom control UIs,
C++-adjacent (zero backend porting), LGPL-clean, and already the language of
the user's desktop (Quickshell).

### Proposed frontend layout

```
frontend-qt/
├─ CMakeLists.txt            # Qt6 QML/Quick; links lib/monix; NOT installed
├─ main.cpp                  # QGuiApplication + QQmlApplicationEngine host
├─ mixerdevice.h/.cpp        # QObject adapter over monix::Device
│     Q_PROPERTY volume/fader/meter… (float 0..1; conversion stays in lib)
│     invokables: setFader()/setRoute()/…
│     signals: stateUpdated()/metersUpdated()
├─ deviceworker.h/.cpp       # QThread polling loop (state 500 ms / meters 70 ms),
│                            # ioctl/ALSA calls off the GUI thread; the dragged
│                            # property is not re-emitted (drag-suppression)
├─ desktopshell.h/.cpp       # capability-detected desktop integration (§6)
└─ qml/
     MainWindow.qml / Fader.qml / Knob.qml / Meter.qml /
     RoutingMatrix.qml / SettingsPanel.qml …
```

Rules:

- QML touches **only** normalized floats and the adapter — no ioctl, no ALSA,
  no entity/selector numbers.
- The adapter stays thin so upstream `lib/` changes drop in without UI work.
- Poll cadence and block-meter reads come straight from the current GUI
  (ARCHITECTURE.md §5); do not change the metering discipline.
- Persistence: keep the `state.conf` format (4-line spec, `app/persist.h`) or
  switch to QSettings — fork owner's choice, format is trivial either way.
- External commands via `QProcess`: `monix-driverctl status/install-current`
  (repair flow), `pw-loopback`/`pactl` (virtual-device split) — logic ported
  from `app/drivercheck.h` / `app/pwsplit.h`.

## 6. Desktop compatibility plan (KDE / GNOME / Hyprland)

Qt Quick windows run identically on all three; the only desktop-sensitive
features are tray, elevation, autostart and theming. **Use capability
detection, never `XDG_CURRENT_DESKTOP` branching.**

| Feature | KDE Plasma | GNOME | Hyprland (fork owner) | Implementation |
|---|---|---|---|---|
| System tray | ✅ SNI built-in | ❌ stock; extension only | ❌ unless quickshell SNI host | `QSystemTrayIcon::isSystemTrayAvailable()`; when false: disable hide-to-tray in settings, close = quit |
| Driver-repair elevation | ✅ polkit agent | ✅ polkit agent | ⚠️ needs an agent (e.g. hyprpolkitagent) | `pkexec` via QProcess, fallback `sudo`; ping `org.freedesktop.PolicyKit1.Authority` for presence |
| Autostart / launcher | ✅ XDG | ✅ XDG | ✅ XDG | `~/.config/autostart/*.desktop` (already the pattern in `appsettings.h`) |
| Theme | custom theme wins | custom theme wins | custom theme wins | keep the dark-violet custom theme; optionally read `QStyleHints::colorScheme()` as the default for a future light mode |
| Global shortcut (optional) | ✅ | ~ (portal) | ~ (portal) | xdg-desktop-portal GlobalShortcuts via DBus; hide the option when unavailable |

Hard rules:

- The mixer window stays a **plain top-level window** — no wlroots
  layer-shell, no Hyprland-specific protocols, no shell embedding. A
  quickshell bar widget (status/volume, open-mixer) is a separate optional
  component, not part of the app.
- Set the Wayland `appId` to `monix` and a proper application display name
  (GNOME/KDE window matching and taskbar icons depend on it).
- Elevation must go through the desktop's polkit agent — no setuid helpers,
  no custom password dialogs.

## 7. Implementation phases

1. **Adapter first**: `mixerdevice.h` wrapping every `Device` method, verified
   against `monixctl` subcommands on hardware (it is the correctness
   reference).
2. **Ugly-but-working**: default QML controls, three-column layout ported from
   `main.cpp`'s logic, polling loop up.
3. **Component design**: Fader (track + handle + gradient meter), Knob (dial
   with scale), RoutingMatrix — the visual rewrite.
4. **Desktop integration**: tray with capability fallback, elevation chain,
   autostart, launcher, theme.
5. **System features**: drivercheck repair dialog, virtual-device panel
   (pw-loopback), persistence, optional global shortcut.

## 8. Risks & open questions

- **Tray on the fork owner's desktop**: no host today — either accept
  no-tray (close = quit) or write a small quickshell SNI host as a side
  project. Do not let this block the app.
- **CI cannot exercise desktops**: only compile checks are possible
  (existing workflow). The `desktopshell` layer must be written so degraded
  paths can be smoke-tested by faking capability flags.
- **Qt runtime on GNOME distros**: package `qt6-base` + `qt6-declarative` +
  `qt6-wayland` as runtime deps; KDE desktops already carry Qt.
- **Persistence format**: decide `state.conf` compatibility vs QSettings
  before phase 2.
- **Dependency footprint**: Qt pulls more than ImGui — acceptable for a
  desktop app, worth stating in the fork's README.
