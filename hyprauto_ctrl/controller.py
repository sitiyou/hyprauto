from __future__ import annotations

import json
import os
import socket
import struct
import tempfile
import threading
import zlib
from pathlib import Path
from typing import Any

import numpy as np


class Job:
    def __init__(self, result: Any = None, error: str | None = None) -> None:
        self._event = threading.Event()
        self._result = result
        self._error = error
        if error is not None or result is not None:
            self._event.set()

    @classmethod
    def _pending(cls) -> Job:
        job = cls()
        job._event.clear()
        return job

    def _finish(self, result: Any = None, error: str | None = None) -> None:
        self._result = result
        self._error = error
        self._event.set()

    def wait(self, timeout: float | None = None) -> Job:
        self._event.wait(timeout)
        return self

    @property
    def done(self) -> bool:
        return self._event.is_set()

    @property
    def succeeded(self) -> bool:
        return self.done and self._error is None

    @property
    def failed(self) -> bool:
        return self.done and self._error is not None

    @property
    def pending(self) -> bool:
        return not self.done

    @property
    def running(self) -> bool:
        return not self.done

    @property
    def error(self) -> str | None:
        return self._error


class JobWithResult(Job):
    def get(self, wait: bool = False) -> Any:
        if wait:
            self.wait()
        return self._result


class HyprAutoController:
    def __init__(
        self,
        target: str | None = None,
        instance: str | None = None,
    ) -> None:
        self.target = target
        self.instance = instance
        self._lock = threading.Lock()
        self._connected = False
        self._socket_path: Path | None = None
        self._cached_image: np.ndarray | None = None

    def _command(self, *args: str) -> str:
        if self._socket_path is None:
            raise RuntimeError("not connected; call post_connection() first")
        request = ("/hyprauto " + " ".join(args)).encode()
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(30)
                connection.connect(str(self._socket_path))
                connection.sendall(request)
                response = bytearray()
                while chunk := connection.recv(65536):
                    response.extend(chunk)
        except OSError as exc:
            raise RuntimeError(f"Hyprland IPC request failed: {exc}") from exc
        output = response.decode(errors="replace").strip()
        if output.startswith("error:"):
            raise RuntimeError(output)
        return output

    def _submit(self, action, result: bool = False) -> Job:
        job = JobWithResult() if result else Job._pending()

        def run() -> None:
            try:
                with self._lock:
                    value = action()
                job._finish(value)
            except Exception as exc:
                job._finish(error=str(exc))

        threading.Thread(target=run, daemon=True).start()
        return job

    def post_connection(self, instance: str | None = None) -> Job:
        def connect() -> str:
            if self._connected:
                self._command("end")
                self._connected = False
            signature = instance or self.instance or os.environ.get("HYPRLAND_INSTANCE_SIGNATURE")
            runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
            if not signature or not runtime_dir:
                raise RuntimeError("Hyprland instance and XDG_RUNTIME_DIR are required")
            path = Path(runtime_dir) / "hypr" / signature / ".socket.sock"
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                    connection.settimeout(5)
                    connection.connect(str(path))
            except OSError as exc:
                raise RuntimeError(f"cannot connect to Hyprland IPC socket {path}: {exc}") from exc
            self.instance = signature
            self._socket_path = path
            if self.target:
                output = self._command("begin", self.target)
                self._connected = True
                return output
            return "ok"

        return self._submit(connect)

    def set_target(self, selector: str) -> Job:
        if not selector:
            return Job(error="target selector must not be empty")

        def select() -> str:
            if self._socket_path is None:
                raise RuntimeError("not connected; call post_connection() first")
            if self._connected:
                self._command("end")
                self._connected = False
            output = self._command("begin", selector)
            self.target = selector
            self._connected = True
            return output

        return self._submit(select)

    def post_click(self, x: int, y: int, contact: int = 0, pressure: int = 1) -> Job:
        button = {0: 272, 1: 273, 2: 274}.get(contact)
        if button is None:
            return Job(error="only mouse contacts 0 (left), 1 (right), and 2 (middle) are supported")

        def click() -> str:
            self._command("move", str(x), str(y))
            self._command("button", str(button), "down")
            return self._command("button", str(button), "up")

        return self._submit(click)

    def post_key_down(self, key: int) -> Job:
        return self._submit(lambda: self._command("key", str(key), "down"))

    def post_key_up(self, key: int) -> Job:
        return self._submit(lambda: self._command("key", str(key), "up"))

    def post_click_key(self, key: int) -> Job:
        def click() -> str:
            self._command("key", str(key), "down")
            return self._command("key", str(key), "up")

        return self._submit(click)

    def post_touch_down(self, x: int, y: int, contact: int = 0, pressure: int = 1) -> Job:
        button = {0: 272, 1: 273, 2: 274}.get(contact)
        if button is None:
            return Job(error="only mouse contacts 0 (left), 1 (right), and 2 (middle) are supported")

        def touch_down() -> str:
            self._command("move", str(x), str(y))
            return self._command("button", str(button), "down")

        return self._submit(touch_down)

    def post_touch_move(self, x: int, y: int, contact: int = 0, pressure: int = 1) -> Job:
        return self._submit(lambda: self._command("move", str(x), str(y)))

    def post_touch_up(self, contact: int = 0) -> Job:
        button = {0: 272, 1: 273, 2: 274}.get(contact)
        if button is None:
            return Job(error="only mouse contacts 0 (left), 1 (right), and 2 (middle) are supported")
        return self._submit(lambda: self._command("button", str(button), "up"))

    def post_screencap(self) -> JobWithResult:
        def capture() -> np.ndarray:
            with tempfile.TemporaryDirectory(prefix="hyprauto-") as directory:
                path = Path(directory) / "capture.png"
                self._command("screenshot", json.dumps(str(path)))
                image = _decode_png(path.read_bytes())
            self._cached_image = image
            return image

        return self._submit(capture, result=True)

    @property
    def cached_image(self) -> np.ndarray:
        if self._cached_image is None:
            raise RuntimeError("no screenshot has been captured")
        return self._cached_image.copy()

    def post_block_input(self, enabled: bool) -> Job:
        mode = "on" if enabled else "off"
        return self._submit(lambda: self._command("block-input", mode))

    def post_status(self) -> JobWithResult:
        return self._submit(lambda: json.loads(self._command("status")), result=True)

    def close(self) -> None:
        with self._lock:
            if self._connected:
                self._command("end")
                self._connected = False
            self._socket_path = None


def _decode_png(data: bytes) -> np.ndarray:
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError("invalid PNG screenshot")
    offset = 8
    width = height = depth = color = 0
    compression = filtering = interlace = 0
    compressed = bytearray()
    while offset + 12 <= len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4 : offset + 8]
        chunk = data[offset + 8 : offset + 8 + length]
        if len(chunk) != length:
            raise RuntimeError("truncated PNG screenshot")
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            compressed.extend(chunk)
        elif kind == b"IEND":
            break
        offset += length + 12
    if depth != 8 or color not in (2, 6) or compression or filtering or interlace:
        raise RuntimeError("unsupported PNG screenshot format")
    if width < 1 or height < 1:
        raise RuntimeError("PNG screenshot has no valid image header")
    channels = 4 if color == 6 else 3
    stride = width * channels
    raw = zlib.decompress(compressed)
    if len(raw) != height * (stride + 1):
        raise RuntimeError("invalid PNG image data length")
    rows = np.empty((height, stride), dtype=np.uint8)
    source = memoryview(raw)
    previous = np.zeros(stride, dtype=np.uint8)
    for y in range(height):
        start = y * (stride + 1)
        filter_type = source[start]
        row = np.frombuffer(source[start + 1 : start + stride + 1], dtype=np.uint8).copy()
        left = np.zeros(stride, dtype=np.uint8)
        above_left = np.zeros(stride, dtype=np.uint8)
        left[channels:] = row[:-channels]
        above_left[channels:] = previous[:-channels]
        if filter_type == 1:
            row = (row.astype(np.uint16) + left).astype(np.uint8)
        elif filter_type == 2:
            row = (row.astype(np.uint16) + previous).astype(np.uint8)
        elif filter_type == 3:
            row = (row.astype(np.uint16) + ((left.astype(np.uint16) + previous) // 2)).astype(np.uint8)
        elif filter_type == 4:
            estimate = left.astype(np.int16) + previous.astype(np.int16) - above_left.astype(np.int16)
            distances = np.stack((abs(estimate - left), abs(estimate - previous), abs(estimate - above_left)))
            predictors = np.stack((left, previous, above_left))
            predictor = np.take_along_axis(predictors, distances.argmin(axis=0)[None, :], axis=0)[0]
            row = (row.astype(np.uint16) + predictor).astype(np.uint8)
        elif filter_type != 0:
            raise RuntimeError(f"unsupported PNG filter {filter_type}")
        rows[y] = row
        previous = row
    pixels = rows.reshape(height, width, channels)
    return pixels[:, :, 2::-1].copy()
