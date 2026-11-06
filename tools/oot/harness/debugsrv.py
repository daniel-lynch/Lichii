"""Minimal client for the emulator's debug server (EDEN_NX2_DEBUG_SERVER, docs/nx2/debug-mcp.md):
one JSON line per request over loopback TCP. The harness uses it for the flash/flicker monitor
(flash_monitor / flash_stats); tools/nx2/debug_mcp.py is the full MCP adapter."""
import json
import socket


class DebugError(Exception):
    pass


class DebugClient:
    def __init__(self, port, host="127.0.0.1"):
        self.addr = (host, port)
        self.sock = None
        self.buf = b""
        self.next_id = 1

    def close(self):
        if self.sock is not None:
            self.sock.close()
        self.sock, self.buf = None, b""

    def call(self, method, params=None, timeout=10):
        try:
            if self.sock is None:
                self.sock = socket.create_connection(self.addr, timeout=5)
            self.sock.settimeout(timeout)
            rid = self.next_id
            self.next_id += 1
            self.sock.sendall(json.dumps({"id": rid, "method": method, "params": params or {}}).encode() + b"\n")
            while b"\n" not in self.buf:
                chunk = self.sock.recv(1 << 20)
                if not chunk:
                    raise ConnectionResetError("the emulator closed the connection")
                self.buf += chunk
            raw, _, self.buf = self.buf.partition(b"\n")
            resp = json.loads(raw)
        except (OSError, ValueError) as e:
            self.close()
            raise DebugError(f"{method}: {e}") from e
        if "error" in resp:
            raise DebugError(f"{method}: {resp['error']}")
        return resp.get("result")


def port_free(port, host="127.0.0.1"):
    with socket.socket() as s:
        try:
            s.bind((host, port))
            return True
        except OSError:
            return False
