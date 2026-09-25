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

STATUS_REQUEST_MSG_TYPE = 18
REBOOT_MSG_TYPE = 19
# Reboots the Pico into BOOTSEL (USB flashing mode) - for when USB serial is
# unavailable but WiFi works; the Pi's pi_flash.py can then flash it.
BOOTSEL_MSG_TYPE = 21
WIFI_RECONNECT_MSG_TYPE = 20

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
