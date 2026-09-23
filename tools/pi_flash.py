#!/usr/bin/env python3
"""
Flash a pre-built .uf2 onto the Pico connected to this Pi, fully unattended.

Forces a *running* Pico into BOOTSEL mode using the standard "1200-baud
touch" (the same convention classic Arduino bootloaders use) instead of
picotool - no picotool install, no libusb, no udev rules, no sudo needed.
Copies the .uf2 onto the auto-mounted RPI-RP2 drive to trigger the flash,
waits for the Pico to reboot back into application firmware, and does a
live functional check (a real NOOP round-trip over the app's own serial
protocol) to confirm the new firmware is actually running - not just that
a file got copied.

Run on the Pi (needs pyserial - the mapping/.venv already has it):
    python3 tools/pi_flash.py <path-to-uf2>

Exit codes: 0 = flashed and verified running. Non-zero = see stderr.
"""
import sys
import os
import time
import glob
import shutil
import subprocess

SERIAL_PORT = "/dev/ttyACM0"
BAUD_NORMAL = 115200
BAUD_MAGIC = 1200  # pico-sdk's PICO_STDIO_USB_RESET_MAGIC_BAUD_RATE default

# Raspberry Pi OS desktop auto-mounts USB mass storage under /media/<user>/;
# other locations covered in case of a different session/mount manager.
# The bootloader drive is labelled RPI-RP2 on an RP2040 (Pico W) and RP2350
# on an RP2350 (Pico 2 W).
MOUNT_GLOB_PATTERNS = [
    f"{root}/{label}"
    for label in ("RPI-RP2", "RP2350")
    for root in ("/media/*", "/run/media/*", "/mnt")
]


def find_mount():
    for pattern in MOUNT_GLOB_PATTERNS:
        matches = glob.glob(pattern)
        if matches:
            return matches[0]
    return None


def wait_for(predicate, timeout, interval=0.2, desc=""):
    end = time.time() + timeout
    while time.time() < end:
        result = predicate()
        if result:
            return result
        time.sleep(interval)
    raise TimeoutError(f"timed out after {timeout}s waiting for: {desc}")


def check_port_free():
    """Best-effort check that nothing else has the serial port open - a
    mapping script, screen/minicom, or a VSCode serial monitor left running
    would block the reset touch from ever reaching the device."""
    try:
        out = subprocess.run(
            ["fuser", SERIAL_PORT], capture_output=True, text=True
        )
        if out.stdout.strip():
            print(
                f"ERROR: {SERIAL_PORT} is held open by PID(s): {out.stdout.strip()}",
                file=sys.stderr,
            )
            print(
                "Close whatever has it open (mapping script, terminal, serial "
                "monitor) and retry.",
                file=sys.stderr,
            )
            return False
    except FileNotFoundError:
        pass  # fuser not installed - proceed optimistically
    return True


def trigger_bootsel():
    import serial

    if not os.path.exists(SERIAL_PORT):
        raise RuntimeError(
            f"{SERIAL_PORT} does not exist - is the Pico plugged in and "
            "running application firmware?"
        )
    ser = serial.Serial(SERIAL_PORT, BAUD_MAGIC)
    time.sleep(0.3)
    ser.close()


def verify_new_firmware():
    """Confirm the flashed firmware is actually alive and responds over the
    real serial protocol, not just that the reboot happened. The NOOP
    round-trip is part of the core protocol (predates this deploy tooling),
    so it's the pass/fail gate. The two heartbeat lines are current-firmware
    diagnostics - reported if present, but not required, so this script
    keeps working if that instrumentation is ever removed."""
    import serial

    ser = serial.Serial(SERIAL_PORT, BAUD_NORMAL, timeout=0.2)
    time.sleep(0.3)
    ser.read(ser.in_waiting or 1)

    end = time.time() + 8
    buf = b""
    while time.time() < end:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
        if b"uptime s:" in buf and b"core1 alive" in buf:
            break
    print(f"  core0 heartbeat: {'seen' if b'uptime s:' in buf else '(not seen)'}")
    print(f"  core1 heartbeat: {'seen' if b'core1 alive' in buf else '(not seen)'}")

    ser.reset_input_buffer()
    t0 = time.time()
    ser.write(bytes([0]))  # NOOP (msg_type 0)
    resp = b""
    end = time.time() + 3
    while time.time() < end:
        chunk = ser.read(256)
        if chunk:
            resp += chunk
        if b"NOOP received" in resp:
            break
    ser.close()

    noop_ok = b"NOOP received" in resp
    print(
        f"  NOOP round-trip: {'OK (%.2fs)' % (time.time() - t0) if noop_ok else 'FAILED'}"
    )
    return noop_ok


def main():
    if len(sys.argv) != 2:
        print("usage: pi_flash.py <path-to-uf2>", file=sys.stderr)
        return 2
    uf2_path = sys.argv[1]
    if not os.path.isfile(uf2_path):
        print(f"ERROR: {uf2_path} not found", file=sys.stderr)
        return 2

    if find_mount() and not os.path.exists(SERIAL_PORT):
        # Already sitting in BOOTSEL (a blank board, or BOOTSEL held at
        # plug-in) - there's no app firmware to reset, just flash it.
        print("[1-2/5] Board is already in BOOTSEL mode, skipping reset")
    else:
        print(f"[1/5] Checking {SERIAL_PORT} is free...")
        if not check_port_free():
            return 1

        print("[2/5] Triggering BOOTSEL reset (1200-baud touch)...")
        try:
            trigger_bootsel()
        except Exception as e:
            print(f"ERROR: could not trigger BOOTSEL reset: {e}", file=sys.stderr)
            return 1

    print("[3/5] Waiting for bootloader drive to mount...")
    try:
        mount = wait_for(find_mount, timeout=15, desc="bootloader drive mount")
    except TimeoutError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    print(f"      found at {mount}")

    print(f"[4/5] Copying firmware ({os.path.getsize(uf2_path)} bytes)...")
    dest = os.path.join(mount, os.path.basename(uf2_path))
    shutil.copy(uf2_path, dest)

    print("[5/5] Waiting for reboot back into application firmware...")
    try:
        wait_for(
            lambda: os.path.exists(SERIAL_PORT) and not os.path.exists(mount),
            timeout=20,
            desc=f"reboot back to {SERIAL_PORT}",
        )
    except TimeoutError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    time.sleep(1.0)  # let the freshly-booted firmware settle

    print("Verifying new firmware is actually running...")
    if not verify_new_firmware():
        print(
            "VERIFY FAILED - firmware was flashed but is not responding as expected",
            file=sys.stderr,
        )
        return 1

    print("DEPLOY OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
