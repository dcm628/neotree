"""
WiFi transport for the tree's command messages - a drop-in for the pyserial
port that neotree_serial's write_tree_* helpers write to:

    import neotree_net, neotree_serial as ns
    tree = neotree_net.connect("192.168.0.213")
    ns.write_tree_all_led(tree, 3, 255, 0, 0)

Each write() is one command message. It is sent with a uint16 little-endian
length prefix and, by default, waits for the tree's ACK (raising on a
rejection). Protocol details: firmware/include/neo_tree_net_server.hpp.
"""
import json
import socket
import struct
import time

DEFAULT_PORT = 7777
PROTOCOL_VERSION = 1

REPLY_ACK = 0x80
REPLY_HELLO = 0x81
REPLY_STATUS = 0x82
REPLY_DESCRIBE = 0x83
# Pushed to clients that subscribe(): the scene / the library, on every change.
REPLY_SCENE = 0x84
REPLY_LIBRARY = 0x85

STATUS_REQUEST_MSG_TYPE = 18
REBOOT_MSG_TYPE = 19
# Reboots the Pico into BOOTSEL (USB flashing mode) - for when USB serial is
# unavailable but WiFi works; the Pi's pi_flash.py can then flash it.
BOOTSEL_MSG_TYPE = 21
# The old demo ids (now modes, run in slot 1 over the Canvas): [22][id], 0 = none.
DEMO_MSG_TYPE = 22
DEMOS = ['none', 'layers', 'wedge', 'sweep_linear', 'sweep_gravity', 'sweep_launch', 'bounce', 'snow', 'orbit',
         'fireworks', 'chain', 'mixer']
WIFI_RECONNECT_MSG_TYPE = 20
# Modes (engine/include/neotree/modes.hpp, director.hpp) - see the tree's
# DESCRIBE reply for mode / parameter / preset indices.
DESCRIBE_MSG_TYPE = 23
SLOT_SET_MSG_TYPE = 24
PARAM_SET_MSG_TYPE = 25
SLOT_END_MSG_TYPE = 26
SLOT_LIFE_MSG_TYPE = 27
INPUT_MSG_TYPE = 28
PRESET_MSG_TYPE = 29
POLICIES = ["loop", "chain", "revert", "remove", "hold"]
# The library (engine/include/neotree/library.hpp) - see library() for
# preset / show indices. Changes are stored in the tree's flash.
LIBRARY_MSG_TYPE = 30
SCENE_SAVE_MSG_TYPE = 31
LIBRARY_DELETE_MSG_TYPE = 32
SHOW_SET_MSG_TYPE = 33
SHOW_PLAY_MSG_TYPE = 34
SHOW_BOOT_MSG_TYPE = 35
SUBSCRIBE_MSG_TYPE = 36
# Diagnostic: times the engine with core1 running / paused; results in the status events.
BENCH_MSG_TYPE = 37
NAME_LEN = 20

STATUS_NAMES = {
    0: "QUEUED",
    1: "QUEUE_FULL",
    2: "UNKNOWN_TYPE",
    3: "BAD_LENGTH",
    4: "NOT_ALLOWED",
}


class NeotreeNetError(Exception):
    pass


class NeotreeNet:
    def __init__(self, host, port=DEFAULT_PORT, timeout=3.0, wait_for_ack=True):
        self.wait_for_ack = wait_for_ack
        self.last_status = None
        self.last_describe = None
        self.last_library = None
        self.pushed = []   # (time, "scene" | "library", dict) - after subscribe()
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        hello = self.read_frame()
        if len(hello) < 2 or hello[0] != REPLY_HELLO:
            raise NeotreeNetError(f"unexpected greeting {hello!r} - is this the tree?")
        self.protocol_version = hello[1]
        if self.protocol_version != PROTOCOL_VERSION:
            raise NeotreeNetError(f"tree speaks protocol v{self.protocol_version}, "
                                  f"this client v{PROTOCOL_VERSION}")

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise NeotreeNetError("connection closed by the tree")
            buf += chunk
        return buf

    def read_frame(self):
        (length,) = struct.unpack("<H", self._recv_exact(2))
        return self._recv_exact(length)

    def send_raw(self, payload):
        """Send one framed message without waiting for anything."""
        self.sock.sendall(struct.pack("<H", len(payload)) + payload)

    def read_ack(self):
        """Returns (command_type, status_name). A STATUS frame arriving first
        (the reply to STATUS_REQUEST, sent just before its ACK) is kept in
        self.last_status."""
        while True:
            frame = self.read_frame()
            if frame and frame[0] == REPLY_STATUS:
                self.last_status = json.loads(frame[1:].decode("utf-8", errors="replace"))
                continue
            if frame and frame[0] == REPLY_DESCRIBE:
                self.last_describe = json.loads(frame[1:].decode("utf-8", errors="replace"))
                continue
            if frame and frame[0] in (REPLY_SCENE, REPLY_LIBRARY):
                self._keep_pushed(frame)
                continue
            if len(frame) != 3 or frame[0] != REPLY_ACK:
                raise NeotreeNetError(f"expected ACK, got {frame!r}")
            return frame[1], STATUS_NAMES.get(frame[2], f"status {frame[2]}")

    def get_status(self):
        """The tree's JSON snapshot of its internals (see
        firmware/include/neo_tree_status.hpp), as a dict."""
        self.last_status = None
        self.write(bytes([STATUS_REQUEST_MSG_TYPE]))
        if self.last_status is None:
            raise NeotreeNetError("no STATUS reply (tree out of memory? try again)")
        return self.last_status

    def describe(self):
        """The tree's built-in modes (with their parameters) and presets."""
        self.last_describe = None
        self.write(bytes([DESCRIBE_MSG_TYPE]))
        if self.last_describe is None:
            raise NeotreeNetError("no DESCRIBE reply (tree out of memory? try again)")
        return self.last_describe

    def _keep_pushed(self, frame):
        data = json.loads(frame[1:].decode("utf-8", errors="replace"))
        if frame[0] == REPLY_LIBRARY:
            self.last_library = data
        self.pushed.append((time.time(), "scene" if frame[0] == REPLY_SCENE else "library", data))

    def subscribe(self, scene=True, library=True):
        """Have the tree push the scene / library on every change (and once
        now). Pushed frames are collected in self.pushed as they're read -
        see wait_pushed()."""
        self.write(bytes([SUBSCRIBE_MSG_TYPE, (1 if scene else 0) | (2 if library else 0)]))

    def wait_pushed(self, seconds):
        """Reads pushed frames for this long; returns those that arrived."""
        start = len(self.pushed)
        end = time.time() + seconds
        old = self.sock.gettimeout()
        try:
            while True:
                left = end - time.time()
                if left <= 0:
                    break
                self.sock.settimeout(left)
                try:
                    frame = self.read_frame()
                except (socket.timeout, TimeoutError):
                    break
                if frame and frame[0] in (REPLY_SCENE, REPLY_LIBRARY):
                    self._keep_pushed(frame)
        finally:
            self.sock.settimeout(old)
        return self.pushed[start:]

    def library(self):
        """Presets (built-ins, then the user's) and shows, the base scene and
        the startup show."""
        self.last_library = None
        self.write(bytes([LIBRARY_MSG_TYPE]))
        if self.last_library is None:
            raise NeotreeNetError("no LIBRARY reply (tree out of memory? try again)")
        return self.last_library

    @staticmethod
    def _name(name):
        raw = name.encode("utf-8")[:NAME_LEN - 1]
        return raw + bytes(NAME_LEN - len(raw))

    def save_scene(self, name=None):
        """Saves the live scene as a preset called name (replacing the user
        preset of that name), or as the base scene if name is None."""
        what = 0 if name is None else 1
        self.write(bytes([SCENE_SAVE_MSG_TYPE, what]) + self._name(name or ""))

    def delete(self, what, index=0):
        """what: "base" (back to the default), "preset" or "show" (by library index)."""
        self.write(bytes([LIBRARY_DELETE_MSG_TYPE, ["base", "preset", "show"].index(what), index]))

    def set_show(self, name, entries, loop=True, shuffle=False):
        """Saves a show: entries = [(preset index, seconds), ...] (seconds 0 =
        the preset's own duration)."""
        body = b"".join(bytes([p]) + struct.pack("<H", int(s)) for p, s in entries)
        self.write(bytes([SHOW_SET_MSG_TYPE, (1 if loop else 0) | (2 if shuffle else 0), len(entries)]) +
                   self._name(name) + body)

    def play_show(self, index):
        """Plays a show (library index); None stops it (the scene stays)."""
        self.write(bytes([SHOW_PLAY_MSG_TYPE, 0xFF if index is None else index]))

    def set_boot_show(self, index):
        """The show to play at power-up; None for the base scene."""
        self.write(bytes([SHOW_BOOT_MSG_TYPE, 0xFF if index is None else index]))

    def set_slot(self, slot, mode_index, fade=True):
        """Puts a mode (by DESCRIBE index; None = empty) in a slot."""
        self.write(bytes([SLOT_SET_MSG_TYPE, slot, 0xFF if mode_index is None else mode_index, 1 if fade else 0]))

    def set_param(self, slot, param_index, value=0.0, rgb=(0, 0, 0)):
        """Sets a running mode's parameter: a number / choice index / toggle, or a color."""
        self.write(bytes([PARAM_SET_MSG_TYPE, slot, param_index]) + struct.pack("<f", float(value)) + bytes(rgb))

    def end_slot(self, slot, op=0):
        """0 = end (its policy applies), 1 = revert (slot 0xFF: the whole scene), 2 = remove, 3 = restart."""
        self.write(bytes([SLOT_END_MSG_TYPE, slot, op]))

    def set_lifecycle(self, slot, duration_s=0, cycles=0, policy="hold", repeats=0, next_mode=None,
                      fade=True, transition_s=1.5):
        self.write(bytes([SLOT_LIFE_MSG_TYPE, slot]) + struct.pack("<HH", int(duration_s), int(cycles)) +
                   bytes([POLICIES.index(policy), repeats, 0xFF if next_mode is None else next_mode,
                          1 if fade else 0, int(round(transition_s * 10))]))

    def preset(self, index):
        self.write(bytes([PRESET_MSG_TYPE, index]))

    def input(self, slot, input_id, value=0.0):
        self.write(bytes([INPUT_MSG_TYPE, slot, input_id]) + struct.pack("<f", float(value)))

    def write(self, payload, queue_full_retries=20, queue_full_backoff_s=0.005):
        """pyserial-compatible: send one command message.

        QUEUE_FULL means the tree is momentarily behind (it only drains
        commands between ~30ms LED frames), so back off and resend rather
        than fail."""
        payload = bytes(payload)
        for attempt in range(queue_full_retries + 1):
            self.send_raw(payload)
            if not self.wait_for_ack:
                break
            cmd_type, status = self.read_ack()
            if status == "QUEUED":
                break
            if status != "QUEUE_FULL" or attempt == queue_full_retries:
                raise NeotreeNetError(f"tree rejected message type {cmd_type}: {status}")
            time.sleep(queue_full_backoff_s * (attempt + 1))
        return len(payload)

    def close(self):
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def connect(host, port=DEFAULT_PORT, **kwargs):
    return NeotreeNet(host, port, **kwargs)
