from __future__ import annotations

import json
import socket
import struct
import threading
from pathlib import Path


class SessionSocket:
    def __init__(self, path: Path) -> None:
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._send_lock = threading.Lock()
        self._state_lock = threading.Lock()
        self._pending: dict[int, tuple[threading.Event, list]] = {}
        self._next_id = 0
        self._error: str | None = None
        self._stopped = threading.Event()
        try:
            self._socket.settimeout(5)
            self._socket.connect(str(path))
            kind, payload = self._read_frame()
            welcome = json.loads(payload)
            if kind != b"J" or welcome.get("id") != 0 or welcome.get("protocol") != 1:
                raise RuntimeError("invalid hyprauto handshake")
            self.session_id = welcome["session_id"]
            self._socket.settimeout(None)
        except Exception:
            self._socket.close()
            raise
        self._reader = threading.Thread(target=self._receive, daemon=True)
        self._heartbeat = threading.Thread(target=self._renew, daemon=True)
        self._reader.start()
        self._heartbeat.start()

    def _read_exact(self, size: int) -> bytes:
        data = bytearray()
        while len(data) < size:
            chunk = self._socket.recv(size - len(data))
            if not chunk:
                raise RuntimeError("hyprauto session disconnected")
            data.extend(chunk)
        return bytes(data)

    def _read_frame(self) -> tuple[bytes, bytes]:
        length = struct.unpack("!I", self._read_exact(4))[0]
        if not 1 <= length <= 1048576:
            raise RuntimeError("invalid hyprauto frame length")
        frame = self._read_exact(length)
        return frame[:1], frame[1:]

    def _fail(self, error: str) -> None:
        with self._state_lock:
            if self._error is None:
                self._error = error
            self._stopped.set()
            for event, result in self._pending.values():
                result.append(RuntimeError(self._error))
                event.set()
            self._pending.clear()
        try:
            self._socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._socket.close()

    def _receive(self) -> None:
        try:
            while not self._stopped.is_set():
                kind, payload = self._read_frame()
                if kind == b"J":
                    response = json.loads(payload)
                    request_id = response["id"]
                    value = RuntimeError(response["error"]) if "error" in response else response["result"].encode()
                elif kind == b"B" and len(payload) >= 8:
                    request_id = struct.unpack("!Q", payload[:8])[0]
                    value = b"data:" + payload[8:]
                else:
                    raise RuntimeError("invalid hyprauto response")
                with self._state_lock:
                    pending = self._pending.pop(request_id, None)
                    if pending is None:
                        raise RuntimeError("unexpected hyprauto response ID")
                    event, result = pending
                    result.append(value)
                    event.set()
        except Exception as exc:
            self._fail(str(exc))

    def request(self, command: str) -> bytes:
        event = threading.Event()
        result: list = []
        with self._send_lock:
            with self._state_lock:
                if self._error is not None:
                    raise RuntimeError(self._error)
                self._next_id += 1
                request_id = self._next_id
                self._pending[request_id] = event, result
            payload = b"J" + json.dumps({"id": request_id, "command": command}).encode()
            if len(payload) > 16384:
                with self._state_lock:
                    self._pending.pop(request_id, None)
                raise RuntimeError("hyprauto request is too large")
            try:
                self._socket.sendall(struct.pack("!I", len(payload)) + payload)
            except OSError as exc:
                self._fail(str(exc))
        if not event.wait(30):
            self._fail("hyprauto request timed out")
            raise RuntimeError("hyprauto request timed out")
        if isinstance(result[0], Exception):
            raise result[0]
        return result[0]

    def _renew(self) -> None:
        while not self._stopped.wait(5):
            try:
                self.request("heartbeat")
            except Exception as exc:
                self._fail(str(exc))
                return

    def close(self) -> None:
        self._fail("hyprauto session closed")
        for thread in (self._reader, self._heartbeat):
            if thread is not threading.current_thread():
                thread.join(timeout=2)
