from __future__ import annotations

import json
import os
import threading
import time
from contextlib import suppress
from pathlib import Path
from typing import Any

import numpy as np

from .transport import SessionSocket


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
    def __init__(self, instance: str | None = None) -> None:
        self.target: str | None = None
        self.instance = instance
        self._lock = threading.Lock()
        self._connected = False
        self._connection: SessionSocket | None = None
        self._cached_image: np.ndarray | None = None

    def _request(self, *args: str) -> bytes:
        if self._connection is None:
            raise RuntimeError("not connected; call post_connection() first")
        return self._connection.request(" ".join(args))

    def _command(self, *args: str) -> str:
        return self._request(*args).decode(errors="replace").strip()

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

    def post_connection(self, target: str | None = None) -> Job:
        def connect() -> str:
            if self._connection is not None:
                self._connection.close()
                self._connection = None
            self._connected = False
            self.target = None
            signature = self.instance or os.environ.get("HYPRLAND_INSTANCE_SIGNATURE")
            runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
            if not signature or not runtime_dir:
                raise RuntimeError("Hyprland instance and XDG_RUNTIME_DIR are required")
            path = Path(runtime_dir) / "hypr" / signature / ".hyprauto.sock"
            self._connection = SessionSocket(path)
            self.instance = signature
            if target:
                output = self._command("set-target", target)
                self.target = target
                self._connected = True
                return output
            self.target = None
            return "ok"

        return self._submit(connect)

    def set_target(self, selector: str) -> Job:
        if not selector:
            return Job(error="target selector must not be empty")

        def select() -> str:
            output = self._command("set-target", selector)
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

    def post_click_key(self, key: int, hold_ms: int = 80) -> Job:
        if hold_ms < 0:
            return Job(error="hold duration must not be negative")

        def click() -> str:
            self._command("key", str(key), "down")
            time.sleep(hold_ms / 1000)
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
            if not self._connected:
                raise RuntimeError("no active session")
            capture_id = str(json.loads(self._command("screenshot"))["id"])
            try:
                deadline = time.monotonic() + 15
                while True:
                    info = json.loads(self._command("screenshot-status", capture_id))
                    if info["state"] == "ready":
                        break
                    if info["state"] == "failed":
                        raise RuntimeError(info["error"])
                    if info["state"] != "pending" or time.monotonic() >= deadline:
                        raise RuntimeError("screenshot failed or timed out")
                    time.sleep(0.002)
                width, height = info["width"], info["height"]
                if (
                    not isinstance(width, int) or not isinstance(height, int)
                    or not (0 < width <= 16384 and 0 < height <= 16384)
                    or width * height > 67108864
                    or info["format"] != "BGRA" or info["size"] != width * height * 4
                ):
                    raise RuntimeError("invalid capture metadata")
                data = bytearray()
                while len(data) < info["size"]:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("screenshot pixel transfer timed out")
                    length = min(65536, info["size"] - len(data))
                    chunk = self._request("screenshot-read", capture_id, str(len(data)), str(length))
                    if not chunk.startswith(b"data:") or len(chunk) != length + 5:
                        raise RuntimeError("invalid screenshot pixel chunk")
                    data.extend(memoryview(chunk)[5:])
                pixels = np.frombuffer(data, dtype=np.uint8).reshape(height, width, 4)
                image = pixels[:, :, :3].copy()
                alpha = pixels[:, :, 3:4]
                if np.any(alpha != 255):
                    image = np.minimum((image.astype(np.uint16) * 255 + alpha // 2) // np.maximum(alpha, 1), 255).astype(np.uint8)
            finally:
                with suppress(RuntimeError):
                    self._command("screenshot-release", capture_id)
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

    @property
    def session_id(self) -> int | None:
        return self._connection.session_id if self._connection is not None else None

    def __enter__(self) -> HyprAutoController:
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()

    def close(self) -> None:
        with self._lock:
            if self._connection is not None:
                try:
                    self._command("end")
                except RuntimeError:
                    pass
                finally:
                    self._connection.close()
                    self._connection = None
            self._connected = False
            self.target = None
