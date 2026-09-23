#!/usr/bin/env python3
"""
Provision the Pico's WiFi credentials over USB serial - they're stored in
the Pico's flash and never compiled into the firmware or written to disk
here. The password is read with a hidden prompt.

Run on the Pi (needs pyserial - the mapping/.venv already has it). From the
desktop, use ssh -t so the password prompt gets a terminal:
    ssh -t treepi "~/workspace/neotree/mapping/.venv/bin/python ~/workspace/neotree/tools/set_wifi.py"

    set_wifi.py                 prompt for SSID + password, save, watch it connect
    set_wifi.py --ssid MyNet    prompt only for the password
    set_wifi.py --status        just show the Pico's current WiFi status
    set_wifi.py --clear         erase stored credentials (Pico goes offline)

The Pico W / Pico 2 W radio is 2.4GHz only.
Wire format: see WIFI_CRED_CHUNK / WIFI_CRED_COMMIT in firmware/src/main.cpp
and firmware/include/neo_tree_wifi.hpp.
"""
import argparse
import getpass
import struct
import sys
import time

import serial

SERIAL_PORT = "/dev/ttyACM0"
BAUD = 115200

WIFI_CRED_CHUNK = 13
WIFI_CRED_COMMIT = 14
FIELD_SSID = 0
FIELD_PASSWORD = 1
CHUNK_MAX_DATA = 32
SSID_MAX = 32
PASSWORD_MAX = 64

ACK_TIMEOUT_S = 3.0
CONNECT_WATCH_S = 50.0  # firmware gives each attempt 30s, then retries after 10s


class LineReader:
    """Accumulates serial output and yields complete lines."""

    def __init__(self, ser):
        self.ser = ser
        self.buf = b""

    def lines_until(self, deadline):
        while time.time() < deadline:
            self.buf += self.ser.read(self.ser.in_waiting or 1)
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                yield line.decode(errors="replace").strip()


def wait_for_line(reader, prefixes, timeout):
    for line in reader.lines_until(time.time() + timeout):
        if line.startswith(prefixes):
            return line
    return None


def send_field(ser, reader, field, data):
    for offset in range(0, len(data), CHUNK_MAX_DATA):
        piece = data[offset:offset + CHUNK_MAX_DATA]
        msg = struct.pack("<BBBB", WIFI_CRED_CHUNK, field, offset, len(piece))
        msg += piece.ljust(CHUNK_MAX_DATA, b"\0")
        ser.write(msg)
        # One message per USB packet: the firmware treats each read burst as
        # one message, so wait for the ack before sending the next.
        line = wait_for_line(reader, ("WIFI_CRED ack", "WIFI_CRED error"), ACK_TIMEOUT_S)
        if line is None:
            sys.exit(f"ERROR: no ack from the Pico for chunk field={field} offset={offset} - "
                     "is it running firmware with WiFi provisioning support?")
        if line.startswith("WIFI_CRED error"):
            sys.exit(f"ERROR: Pico rejected chunk: {line}")


def commit(ser, reader, ssid_len, password_len):
    ser.write(struct.pack("<BBB", WIFI_CRED_COMMIT, ssid_len, password_len))
    line = wait_for_line(reader, ("WIFI_CRED saved", "WIFI_CRED cleared", "WIFI_CRED error"), ACK_TIMEOUT_S)
    if line is None:
        sys.exit("ERROR: no response from the Pico to the commit")
    if line.startswith("WIFI_CRED error"):
        sys.exit(f"ERROR: Pico rejected the credentials: {line}")
    print(f"Pico: {line}")


def watch_connect(reader):
    """Echo the firmware's wifi: lines until it connects or the watch window ends."""
    print("Watching the Pico connect...")
    deadline = time.time() + CONNECT_WATCH_S
    for line in reader.lines_until(deadline):
        if line.startswith("wifi:"):
            print(f"  {line}")
            if "hostname" in line and " mac " in line:  # last line of the connected report
                return True
    return False


def show_status(reader):
    line = wait_for_line(reader, ("core1 alive",), 7.0)
    if line is None:
        sys.exit("ERROR: no heartbeat from the Pico within 7s")
    print(line.split("wifi: ", 1)[-1] if "wifi: " in line else line)


def prompt_credentials(args):
    ssid = args.ssid if args.ssid is not None else input("SSID: ")
    ssid_bytes = ssid.encode("utf-8")
    if not 1 <= len(ssid_bytes) <= SSID_MAX:
        sys.exit(f"ERROR: SSID must be 1-{SSID_MAX} bytes")
    password = getpass.getpass("Password (blank for an open network): ")
    if password:
        if getpass.getpass("Confirm password: ") != password:
            sys.exit("ERROR: passwords don't match")
    password_bytes = password.encode("utf-8")
    if password_bytes and not 8 <= len(password_bytes) <= PASSWORD_MAX:
        sys.exit("ERROR: a WPA2 password must be 8-63 characters (or a 64-hex-digit key)")
    return ssid_bytes, password_bytes


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default=SERIAL_PORT)
    parser.add_argument("--ssid", help="network name (prompted for if omitted)")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--status", action="store_true", help="show current WiFi status and exit")
    group.add_argument("--clear", action="store_true", help="erase stored credentials")
    args = parser.parse_args()

    # Prompt before opening the port so a slow typist doesn't back up the
    # Pico's output buffer.
    creds = None if (args.status or args.clear) else prompt_credentials(args)

    ser = serial.Serial(args.port, BAUD, timeout=0.2)
    try:
        time.sleep(0.3)
        ser.reset_input_buffer()
        reader = LineReader(ser)

        if args.status:
            show_status(reader)
            return 0
        if args.clear:
            commit(ser, reader, 0, 0)
            return 0

        ssid_bytes, password_bytes = creds
        send_field(ser, reader, FIELD_SSID, ssid_bytes)
        if password_bytes:
            send_field(ser, reader, FIELD_PASSWORD, password_bytes)
        commit(ser, reader, len(ssid_bytes), len(password_bytes))
        if not watch_connect(reader):
            print(f"Not connected within {CONNECT_WATCH_S:.0f}s. The credentials are saved and the Pico "
                  "keeps retrying - check the SSID/password and that the network is 2.4GHz.")
            return 1
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
