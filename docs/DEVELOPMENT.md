# Development Workflow & Environment Setup

**Status:** First draft · **Owner:** Dan · **Last updated:** 2026-09-20

**Related:** [`ARCHITECTURE.md`](./ARCHITECTURE.md) — what the system is
(hardware, subsystems, interfaces). This document is *how you work on it*.

> This document defines how the project is developed, versioned, and deployed —
> the plumbing, not the features. It replaces the previous ad-hoc workflow
> (firmware edited on a weak Windows laptop and hand-flashed over USB; Python
> written directly on the Pi in Thonny; no version control). The goal is a
> single, repeatable setup where you develop from your desktop, everything lives
> in git on GitHub, and firmware deploys to the Pico remotely through the Pi 4.

---

## Progress (as of 2026-09-20)

Desktop development environment is **set up and verified**:

- ✅ **Firmware is on GitHub** (`github.com/dcm628/neotree`) and cloned to the
  desktop at `~/workspace/neotree`. The repo already carries a `firmware/`
  project, root + firmware `.gitignore`, and a README.
- ✅ **Git installed** on the desktop (was missing; installed via winget).
- ✅ **Firmware toolchain working on the desktop** via the official Raspberry Pi
  Pico VSCode extension — it manages the SDK (2.1.0), ARM GCC (13.3), CMake,
  Ninja, and picotool. This supersedes the manual/command-line toolchain option
  in Phase 3 below; no separately-cloned `pico-sdk` is needed.
- ✅ **Clean build verified** — produces `neo_tree.uf2` (plus `.elf/.bin/.hex`).
- ✅ **Remote-SSH into the Pi for Python (Phase 2)** — working, key-based, via
  the `treepi` alias.
- ✅ **Remote deploy pipeline (Phase 4) — done, one command:** `tools/deploy.ps1`
  builds on the desktop, ships the `.uf2` to the Pi, and flashes + verifies it
  runs, with no physical access to either machine. Built differently than
  originally planned below — see the note at the top of Phase 4 for why.
- ℹ️ **Board target:** the tree runs `pico_w` (RP2040); project is migrating to
  `pico2_w` (RP2350). **As of 2026-09-23** a bare Pico 2 W (no LEDs) is the
  testbed on the Pi's USB in place of the tree's Pico, building in its own
  directory (`cmake -B build_pico2w -G Ninja -DPICO_BOARD=pico2_w`, deploy with
  `tools/deploy.ps1 -Board pico2_w`). Both boards build from the same source.
- ✅ **WiFi (2026-09-23):** CYW43 init restored (its old intermittent hang was a
  PIO state-machine race with the WS2812 init — fixed, 20/20 clean boots), and
  the board joins WiFi as a station with auto-reconnect, credentials
  provisioned over USB (§7). Power-save is off for latency (~10ms ping).
  Connection events and the heartbeat report status over serial.
- ✅ **WiFi control, phase A (2026-09-23):**
  - The tree runs a TCP command server (port 7777) and answers to
    `neotree.local`. From Python, `mapping/neotree_net.connect("neotree.local")`
    works with every `neotree_serial.write_tree_*` helper.
  - The Android app lives in `android/`. Build and install from the desktop
    (the phone must be paired for wireless debugging in Android Studio):
    ```powershell
    $env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"
    cd android; .\gradlew.bat assembleDebug
    & "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" install -r app\build\outputs\apk\debug\app-debug.apk
    ```
  - A cold build takes ~2.5 min; incremental builds take seconds.

Still to do: camera calibration / 3D coordinate mapping, coordinate-driven
volumetric rendering, the control interface, and the RP2040→RP2350 migration.
The phased plan below remains the roadmap for what's done.

---

## 1. Target Workflow (the end state)

```
        ┌───────────────────────────────────────────────────────┐
        │  DESKTOP (Windows) — your one cockpit                  │
        │                                                        │
        │  VSCode window A: LOCAL  → /firmware                   │
        │     Pico SDK + Pico extension, builds .uf2/.elf here   │
        │                                                        │
        │  VSCode window B: REMOTE-SSH → Pi 4  → /mapping etc.   │
        │     edits & runs Python on the Pi, no more Thonny      │
        └───────┬───────────────────────────────┬───────────────┘
                │ git push/pull                  │ SSH + scp
                ▼                                │
        ┌───────────────┐                        ▼
        │    GitHub      │              ┌────────────────────────┐
        │ (private repo) │◀── git ──────│   RASPBERRY PI 4        │
        │  origin        │   push/pull  │   "deploy hub"          │
        └───────────────┘              │                          │
                                        │  • runs /mapping Python  │
                                        │  • picotool flashes Pico │
                                        │  • serial-commands Pico  │
                                        └───────────┬──────────────┘
                                                    │ USB (CDC serial + flashing)
                                                    ▼
                                            ┌────────────────┐
                                            │  Pico 2 W      │
                                            │  → LED strings │
                                            └────────────────┘
```

The mental model in one sentence: **you touch only the desktop.** Firmware
compiles locally on the desktop and is shipped to the Pi, which flashes the Pico
over the same USB cable it already uses to send mapping commands. Python is
edited on the desktop but executes on the Pi via Remote-SSH. Git ties it all
together, with GitHub as the source of truth and your first real offsite backup.

**Hardware prerequisite — confirmed ✅:** the Pi 4 talks to the Pico over the
Pico's **native USB port**. That single connection carries both the serial
command channel (USB CDC, for the mapping sequence) *and* picotool flashing, so
remote deploy works over the cable you already have — no rewiring needed.

---

## 2. Repository Strategy

**Host:** GitHub, single **private** repository (monorepo). One repo keeps the
firmware, mapping code, coordinate maps, and docs versioned together and moving
in lockstep — important when a change to the LED ordering or map format has to
land on both sides at once.

Proposed layout (from the architecture doc):

```
/firmware        # C++ Pico SDK project (built on desktop)
/mapping         # Python: capture, centroid, calibration, 3D solve (runs on Pi)
/coord-map       # generated map artifacts (master CSV/JSON; firmware header/blob)
/web-ui          # static control UI (later)
/docs            # ARCHITECTURE.md, this file, future design docs
/tools           # deploy scripts, helpers
README.md
.gitignore
```

**Branching (keep it light — this is a solo project):** work on `main`
directly for small changes; cut a short-lived `feature/<thing>` branch for
anything you might want to abandon or that spans multiple sessions; merge back
when it works. No heavyweight process. Tag a commit (e.g. `v-tree-2026`) each
time you get a working seasonal setup so you can always return to "it worked last
Christmas."

**What must NOT go in git** (`.gitignore`):

- Firmware build output: `build/`, `*.uf2`, `*.elf`, `*.bin`, `*.dis`
- The Pico SDK itself (reference it by env var / submodule, don't vendor a copy)
- Python: `__pycache__/`, `*.pyc`, `.venv/`, `venv/`
- Large capture artifacts from mapping runs (raw images) — keep these out of git;
  store the *derived* coordinate map instead. **[DECIDE]** whether to keep a
  sample capture set via Git LFS or just outside the repo.
- Secrets: WiFi credentials, any tokens — use an untracked config file (see §7).

---

## 3. Phased Plan

Each phase is independently useful; do them in order. Phases 1–2 give you version
control and remote editing immediately; phase 4 is the payoff (one-command
remote deploy).

### Phase 1 — Git + GitHub foundation

Goal: everything under version control on GitHub, consolidated into the monorepo.

1. **Create the private repo** on GitHub (empty, no auto-README so the first push
   is clean).
2. **Consolidate existing code** into the layout in §2:
   - Gather the firmware's scattered local files into `/firmware`.
   - Copy the Python from the Pi into `/mapping`.
   - Drop in `ARCHITECTURE.md` and this file under `/docs`.
3. **Add `.gitignore`** (§2) *before* the first commit so build junk never enters
   history.
4. **First commit + push** from the desktop.
5. **Auth from both machines:**
   - Desktop: install [GitHub CLI](https://cli.github.com/) and run `gh auth
     login`, or set up an SSH key and add it to GitHub.
   - Pi: generate an SSH key (`ssh-keygen -t ed25519`), add the public key to
     GitHub as an authorized key (or use `gh auth login` on the Pi). This lets
     the Pi pull the `/mapping` code directly.

> Migration note: once this is pushed and verified, the old ad-hoc file backups
> are superseded — GitHub is now your history and offsite copy. Keep the last
> known-good local copy around until you've confirmed the repo builds/runs, then
> retire it.

### Phase 2 — Remote development into the Pi 4 (Python)

Goal: edit Python on the desktop in VSCode, run it on the Pi. Retire Thonny.

1. **Enable SSH on the Pi:** `sudo raspi-config` → Interface Options → SSH (or it
   may already be on). Give the Pi a stable address — either a DHCP reservation
   on your router or rely on mDNS (`raspberrypi.local` / `<hostname>.local`).
   **[CONFIRM]** the Pi's hostname.
2. **Passwordless SSH from desktop → Pi:** on the desktop (Windows has OpenSSH
   built in), `ssh-keygen` if you haven't, then `ssh-copy-id`-equivalent (copy
   your public key into the Pi's `~/.ssh/authorized_keys`). Add a `~/.ssh/config`
   entry so you can just `ssh tree-pi`:
   ```
   Host tree-pi
       HostName raspberrypi.local
       User pi
   ```
3. **VSCode Remote-SSH:** install the *Remote - SSH* extension on the desktop,
   connect to `tree-pi`, and open the cloned repo folder on the Pi. VSCode runs
   its server on the Pi; you edit as if local. Install the Python extension *in
   the remote context*.
4. **Clone the repo on the Pi** (into e.g. `~/neopixel-tree`) and do `/mapping`
   work there.
5. **Proper Python environment (off Thonny):**
   - Create a venv on the Pi: `python3 -m venv .venv && source .venv/bin/activate`
   - Capture dependencies in `/mapping/requirements.txt` (expect NumPy, OpenCV,
     camera libs). Install with `pip install -r requirements.txt`.
   - Point VSCode's Python interpreter at `.venv`.
   - Anything OpenCV/USB-camera-specific must run on the Pi (that's where the
     cameras are), which Remote-SSH handles naturally.

### Phase 3 — Firmware toolchain on the desktop

Goal: build the firmware on the good desktop instead of the weak laptop.

1. **Install the Pico toolchain on Windows:** the official *Raspberry Pi Pico*
   VSCode extension bundles CMake, the ARM GCC toolchain, and the SDK, and it
   supports the RP2350 / Pico 2 W. This is the same extension you used before,
   now on capable hardware.
2. **Open `/firmware`** in a *local* VSCode window (separate from the Remote-SSH
   window in Phase 2 — two windows, one for each side).
3. **Set the board to `pico2_w`** (RP2350, wireless) in the extension / CMake
   config so WiFi and the right SDK bits are pulled in.
4. **Build** and confirm it reproduces your current working firmware, producing
   `firmware.uf2` and `firmware.elf` under `build/`.
5. **One-time sanity flash by hand** (BOOTSEL + drag the .uf2, or `picotool` over
   USB) to prove the desktop build runs on the tree before automating deploy.

### Phase 4 — Remote deploy pipeline (desktop build → Pi flashes Pico) — ✅ done

Goal: after editing firmware on the desktop, one command builds and flashes the
Pico with no physical access. **Implemented as `tools/deploy.ps1` +
`tools/pi_flash.py`, built and verified 2026-09-21.**

**Built differently than originally planned below (kept for history).** The
picotool-based plan (steps 1–2 as first written) turned out not to be
viable: `picotool` isn't packaged for the Pi's Raspberry Pi OS repos or
Debian's, and building it from source pulls in a full pico-sdk checkout plus
libusb-dev just for one command. Instead, the deploy script uses a feature
pico-sdk's `stdio_usb` already enables by default: the **"1200-baud touch"**
reset-to-BOOTSEL — the same convention classic Arduino bootloaders use.
Opening the Pico's existing CDC serial port at 1200 baud and closing it
immediately (`pico/stdio_usb/reset_interface.c`,
`PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE`, default `1`, magic rate `1200`)
triggers the exact same reset-to-BOOTSEL that `picotool load -f` would have.
No picotool, no libusb, no udev rules, no sudo — just pyserial, which the
mapping venv already has. Once in BOOTSEL mode, Raspberry Pi OS's desktop
session auto-mounts the Pico as a plain `RPI-RP2` USB drive, and a normal
file copy onto it (no special tooling) triggers the actual flash, exactly
like the manual BOOTSEL-drag-and-drop everyone already knows.

1. ~~Install picotool on the Pi~~ — not needed; see above.
2. ~~udev rules on the Pi~~ — not needed; the mount-and-copy approach uses
   nothing but the desktop session's normal auto-mount handling.
3. **Reset-to-BOOTSEL in the firmware** — no firmware change was needed at
   all; `pico_enable_stdio_usb(neo_tree 1)` already enables the baud-rate
   reset trick by default. (The vendor-interface/picotool reset path is also
   enabled by default alongside it, so `picotool load -f` would still work
   too if picotool ever gets installed some other way — the two mechanisms
   aren't exclusive.)
4. **Mind the shared USB port — still applies.** `tools/pi_flash.py` checks
   with `fuser /dev/ttyACM0` before doing anything and refuses to proceed
   (with a clear message) if something else has the port open — a mapping
   script, `screen`/`minicom`, or a VSCode serial monitor left running.
5. **Deploy script — `tools/deploy.ps1`** (desktop) + **`tools/pi_flash.py`**
   (Pi, invoked over SSH). Sequence:
   ```powershell
   # on desktop:
   .\tools\deploy.ps1
   ```
   which does, end to end:
   ```
   ninja (build/)                                   # desktop
   scp build/neo_tree.uf2  treepi:/tmp/neo_tree.uf2  # desktop -> Pi
   ssh treepi python3 tools/pi_flash.py /tmp/neo_tree.uf2
   #   on the Pi: 1200-baud touch -> BOOTSEL -> wait for RPI-RP2 mount
   #   -> copy .uf2 onto it -> wait for reboot back to /dev/ttyACM0
   #   -> live NOOP round-trip over the real protocol to confirm it's
   #      actually running the new firmware, not just that a file copied
   ```
6. **VSCode task buttons** — not done yet; `tools/deploy.ps1` runs fine
   standalone from a terminal, a `tasks.json` entry is a nice-to-have on top.
7. **Test the full loop — done.** Verified live against real hardware: build
   → deploy → the Pico visibly drives the actual LED strings correctly
   (single-LED walk, confirmed by eye through the mapping capture script's
   camera feed).

### Phase 5 — Quality of life & hardening

Do these once the core loop works; none are blockers.

- **README** with the from-scratch setup steps (so future-you, or a fresh
  machine, can rebuild the environment).
- **Serial monitor over SSH** helper (`/tools/`) to tail the Pico's logs.
- **Consistent formatting/linting:** `clang-format` for firmware, `black` + a
  linter for Python, so diffs stay clean.
- **Optional CI (GitHub Actions):** build `firmware.uf2` on every push to catch
  build breaks. Actions can compile the Pico SDK in the cloud; it won't flash
  hardware, but a green build badge is cheap insurance. **[OPTIONAL]**
- **Backups beyond git:** GitHub covers code; separately keep the generated
  coordinate map(s) somewhere durable since regenerating one means re-scanning
  the whole tree.

---

## 4. Upgrade Path (deferred, not now)

- **SWD debugging on the Pi:** if you later want breakpoints and live debugging
  (invaluable for the volumetric renderer and simulations), wire ~3 lines from
  the Pi's GPIO to the Pico's SWD pins and run OpenOCD + gdb on the Pi. This is
  the natural evolution of the "Pi as deploy hub" setup and coexists with the
  picotool/USB path. Chosen now: picotool/USB (no extra wiring); SWD is the
  future upgrade when debugging pain justifies it.

---

## 5. Division of Labor (which machine does what)

| Task | Machine | Tooling |
|---|---|---|
| Edit firmware | Desktop (local VSCode) | Pico extension, CMake, ARM GCC |
| Build firmware → `.uf2` | Desktop | Pico SDK |
| Flash Pico | Pi 4 | picotool over USB |
| Edit Python | Desktop (Remote-SSH) | VSCode Python ext (remote) |
| Run mapping / cameras | Pi 4 | Python venv, OpenCV |
| Send serial commands to Pico | Pi 4 | existing USB-serial protocol |
| Version control | Both → GitHub | git, gh CLI |
| Weak Windows laptop | **Retired** | — |

---

## 6. Serial Command Channel (existing, keep)

The Pi 4 already commands the Pico over USB serial to run the mapping sequence
(all LEDs off, one LED white, snap). That stays as-is and becomes one of the Pi's
two runtime jobs (commanding + flashing) over the same port. Two notes carried
into the plan: (1) the flash step must not fight the serial step for the port
(§3, Phase 4.4); (2) this command protocol will be worth documenting in its own
right later, since it's also how the mapping capture drives the tree — but that's
a technical-design item, out of scope for this workflow doc.

---

## 7. Secrets & Config

WiFi credentials and anything else sensitive should **never** be committed.

- **Firmware WiFi credentials — decided (2026-09-23): provisioned over USB,
  never compiled in.** `tools/set_wifi.py` (run on the Pi) prompts for the
  SSID and a hidden password and sends them over the existing serial protocol
  (`WIFI_CRED_CHUNK` / `WIFI_CRED_COMMIT`, ≤36-byte messages so each fits one
  USB packet, acked one at a time). The firmware stores them in their own
  flash sector directly below the LED position config, where they survive
  reflashing, and reconnects immediately. Nothing is written to disk on the
  desktop or Pi, and builds/`.uf2`s carry no secrets. The password sits
  unencrypted in the Pico's flash — accepted, the board never leaves the house.
  ```powershell
  ssh -t treepi "~/workspace/neotree/mapping/.venv/bin/python ~/workspace/neotree/tools/set_wifi.py"
  # also: --status, --clear, --ssid NAME
  ```
- Python: an untracked `config.local.py` / `.env`, with a committed example.

---

## 8. Open Items / To Confirm

Resolved:

- ✅ Pi↔Pico link is the Pico's *native USB* — remote flashing works over the
  existing cable.
- ✅ Firmware will get the USB stdio / reset-interface build flags (Dan) so
  `picotool load -f` can reboot into BOOTSEL remotely.

Still open:

- **[CONFIRM]** Pi hostname / how the desktop will address it (`.local` vs. fixed
  IP).
- **[DECIDE]** raw mapping-capture images: Git LFS, or keep out of the repo.
- **[DECIDE]** secrets mechanism (§7) — finalize when the WiFi phase begins.
- **[OPTIONAL]** GitHub Actions CI to build the firmware on push.

---

## 9. Suggested First Session (concrete starting point)

When you're ready to start executing (and once you connect me to the machines):

1. Create the private GitHub repo.
2. Scaffold the monorepo layout + `.gitignore` + a starter README on the desktop;
   move the firmware files in; first commit + push.
3. Pull the Python off the Pi into `/mapping`, commit.
4. Set up SSH keys desktop→Pi and both machines→GitHub; get Remote-SSH connecting
   to the Pi.
5. Stand up the firmware toolchain on the desktop and reproduce a working build.
6. Then build the deploy script and close the loop.

I can drive most of steps 2–6 directly once I'm connected to the desktop and the
Pi.
