import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import zlib
from pathlib import Path

TESTS = Path(__file__).resolve().parent


def png_pixel(path, x, y):
    data = path.read_bytes()
    assert data.startswith(b"\x89PNG\r\n\x1a\n")
    width, height, depth, color, _, _, _ = struct.unpack(">IIBBBBB", data[16:29])
    assert depth == 8 and color in (2, 6) and 0 <= x < width and 0 <= y < height
    channels = 4 if color == 6 else 3
    compressed = bytearray()
    offset = 8
    while offset < len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        if kind == b"IDAT":
            compressed.extend(data[offset + 8:offset + 8 + length])
        offset += length + 12
    raw = zlib.decompress(compressed)
    stride = width * channels
    previous = bytearray(stride)
    for row_index in range(height):
        start = row_index * (stride + 1)
        filt = raw[start]
        row = bytearray(raw[start + 1:start + stride + 1])
        for index in range(stride):
            left = row[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filt == 1:
                row[index] = (row[index] + left) & 255
            elif filt == 2:
                row[index] = (row[index] + above) & 255
            elif filt == 3:
                row[index] = (row[index] + (left + above) // 2) & 255
            elif filt == 4:
                estimate = left + above - upper_left
                distances = (abs(estimate - left), abs(estimate - above), abs(estimate - upper_left))
                predictor = (left, above, upper_left)[distances.index(min(distances))]
                row[index] = (row[index] + predictor) & 255
            else:
                assert filt == 0
        if row_index == y:
            pixel = tuple(row[x * channels:x * channels + channels])
            return pixel + ((255,) if channels == 3 else ())
        previous = row
    raise AssertionError("PNG row is missing")


def png_size(path):
    data = path.read_bytes()
    assert data.startswith(b"\x89PNG\r\n\x1a\n")
    return struct.unpack(">II", data[16:24])


def wait_for(predicate, description, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.02)
    raise AssertionError(f"Timed out: {description}")


class Client:
    def __init__(self, name, env, logs):
        self.name = name
        self.events = []
        self.condition = threading.Condition()
        log_name = f"{name}-{time.monotonic_ns()}"
        self.stderr = (logs / f"{log_name}.stderr").open("w")
        self.process = subprocess.Popen(
            ["input-client", name], env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr,
            text=True, bufsize=1,
        )
        self.log = (logs / f"{log_name}.events").open("w")
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        try:
            wait_for(lambda: any(e.startswith("configured ") for e in self.events), f"{name} mapped")
        except BaseException:
            self.close()
            raise

    def read(self):
        for line in self.process.stdout:
            event = line.rstrip()
            with self.condition:
                self.events.append(event)
                self.log.write(line)
                self.log.flush()
                self.condition.notify_all()

    def request(self, request, reply):
        with self.condition:
            start = len(self.events)
            self.process.stdin.write(request + "\n")
            self.process.stdin.flush()
            if not self.condition.wait_for(lambda: reply in self.events[start:], timeout=5):
                raise AssertionError(f"{self.name}: missing {reply}; exit={self.process.poll()}")
        return self.events[:]

    def sync(self):
        return self.request("sync", "sync")

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.reader.join(timeout=2)
        self.log.close()
        self.stderr.close()


def run(binary, build_dir, plugin):
    artifacts = build_dir / "test-artifacts"
    artifacts.mkdir(exist_ok=True)
    logs = Path(tempfile.mkdtemp(prefix="test-", dir=artifacts))
    clients = []
    compositor = None
    compositor_log = (logs / "hyprland.log").open("w")
    with tempfile.TemporaryDirectory(prefix="ai-", dir="/tmp") as isolation:
        runtime = Path(isolation) / "r"
        runtime.mkdir(mode=0o700)
        env = os.environ.copy()
        for name in ("WAYLAND_DISPLAY", "DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "SWAYSOCK",
                     "DBUS_SESSION_BUS_ADDRESS", "XAUTHORITY", "MANAGERPID", "NOTIFY_SOCKET"):
            env.pop(name, None)
        env.update({
            "PATH": str(build_dir) + os.pathsep + env.get("PATH", ""),
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_CONFIG_HOME": str(Path(isolation) / "config"),
            "XDG_CACHE_HOME": str(Path(isolation) / "cache"),
            "HYPRLAND_HEADLESS_ONLY": "1",
            "HYPRLAND_NO_SD_VARS": "1",
            "HYPRLAND_NO_SD_NOTIFY": "1",
            "HYPRLAND_NO_SD_TARGET": "1",
        })
        try:
            assert shutil.which("bwrap"), "bwrap is required for device isolation"
            sandbox = ["bwrap", "--die-with-parent", "--unshare-pid", "--ro-bind", "/", "/",
                       "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/run",
                       "--bind", isolation, isolation]
            nodes = sorted(Path("/dev/dri").glob("renderD*"))
            assert nodes, "A DRM render node is required"
            node = nodes[0]
            sandbox.extend(("--dev-bind", str(node), str(node)))
            compositor = subprocess.Popen(
                sandbox + ["env", f"LD_PRELOAD={build_dir / 'headless.so'}",
                           f"AUTO_INPUT_RENDER_NODE={node}", str(binary), "--config", str(TESTS / "test.lua")],
                env=env, stdout=compositor_log, stderr=subprocess.STDOUT,
            )
            ipc = wait_for(lambda: next(iter(runtime.glob("hypr/*/.socket.sock")), None), "headless IPC")

            def command(request):
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                    connection.settimeout(5)
                    connection.connect(str(ipc))
                    connection.sendall(request.encode())
                    chunks = []
                    while chunk := connection.recv(65536):
                        chunks.append(chunk)
                return b"".join(chunks).decode().strip()

            def ok(request):
                response = command(request)
                assert response == "ok", (request, response)

            def auto(request):
                ok("/hyprauto " + request)

            def status():
                return json.loads(command("/hyprauto status"))

            def sync_all():
                for client in clients:
                    if client.process.poll() is None:
                        client.sync()

            def client_info(name):
                return next(c for c in json.loads(command("j/clients")) if c["class"] == name)

            def host_move(name, local_x, local_y):
                x, y = client_info(name)["at"]
                ok(f"/dispatch hl.dsp.cursor.move({{ x = {x + local_x}, y = {y + local_y} }})")
                sync_all()

            def focus(name):
                ok(f"/dispatch hl.dsp.focus({{ window = 'class:{name}' }})")
                host_move(name, 100, 100)
                assert status()["host"]["window"] == name

            def host_key(code, down):
                ok(f"/eval hl.plugin.test.keybind({int(down)}, 0, {code + 8})")

            def host_click(code, down):
                ok(f"/eval hl.plugin.test.click({code}, {int(down)})")

            def unchanged(action):
                before = status()["host"]
                action()
                sync_all()
                assert status()["host"] == before, (before, status()["host"])

            def check(message):
                print("PASS:", message, flush=True)

            wait_for(lambda: list(runtime.glob("wayland-*")), "headless Wayland socket")
            displays = [p for p in runtime.glob("wayland-*") if not p.name.endswith(".lock")]
            assert len(displays) == 1
            env["WAYLAND_DISPLAY"] = displays[0].name
            ok(f"/plugin load {build_dir / 'hyprtestplugin.so'}")
            ok(f"/plugin load {plugin}")
            if not json.loads(command("j/monitors")):
                ok("/output create headless HEADLESS-1")
            monitors = json.loads(command("j/monitors"))
            assert monitors and all(m["name"].startswith("HEADLESS") for m in monitors), monitors
            errors = command("/configerrors")
            assert not errors or errors == "ok", errors
            for name in ("target", "host-a", "host-b"):
                clients.append(Client(name, env, logs))
            target, host_a, host_b = clients
            wait_for(lambda: len(json.loads(command("j/clients"))) == 3, "three windows")
            focus("target")
            idle_start = len(target.sync())
            host_key(48, True)
            host_key(48, False)
            host_move("target", 120, 130)
            host_click(273, True)
            host_click(273, False)
            ok("/eval hl.plugin.test.scroll(12)")
            idle = target.sync()[idle_start:]
            assert any(e.startswith("key 48 1 ") for e in idle), idle
            assert "button 273 1" in idle and "button 273 0" in idle, idle
            assert any(e.startswith("motion ") for e in idle), idle
            assert any(e.startswith("axis ") for e in idle), idle
            assert not status()["active"] and not status()["block_input"]
            assert command("/hyprauto block-input on").startswith("error:")
            check("loading hooks without a session leaves normal keyboard, pointer and scroll usable")
            focus("host-a")
            start = len(target.sync())
            unchanged(lambda: auto("begin class:target"))
            assert not status()["block_input"]
            events = target.events[start:]
            assert events.count("keyboard-enter") == 1, events
            assert "pointer-enter 1.000 1.000" in events, events
            assert "active 1" not in events, events
            screenshot = runtime / "target capture.png"
            captured = command(f'/hyprauto screenshot "{screenshot}"')
            assert captured == "ok: 320x240 " + str(screenshot), captured
            assert png_size(screenshot) == (320, 240)
            assert png_pixel(screenshot, 160, 120)[:3] == (48, 80, 112)
            check("background target screenshot writes correctly cropped PNG pixels without changing host focus")
            check("background session enters once without changing host focus or activation")

            auto("key 42 down")
            auto("key 30 down")
            auto("button 272 down")
            unchanged(lambda: auto("move 40 50"))
            target.sync()
            assert "key 30 1 65" in target.events, target.events
            assert "motion 40.000 50.000" in target.events
            held_start = len(target.events)
            focus("host-b")
            host_key(48, True)
            host_key(48, False)
            host_click(273, True)
            host_click(273, False)
            sync_all()
            assert not [e for e in target.events[held_start:] if e != "sync"], target.events[held_start:]
            assert any(e.startswith("key 48 1 ") for e in host_b.events)
            assert "button 273 1" in host_b.events
            unchanged(lambda: auto("key 30 up"))
            unchanged(lambda: auto("key 42 up"))
            unchanged(lambda: auto("button 272 up"))
            assert "key 30 0 65" in target.events
            assert "button 272 0" in target.events
            for host in (host_a, host_b):
                assert not any(e.startswith(("key 30 ", "key 42 ", "button 272 ")) for e in host.events), host.events
            check("held keys/buttons survive host focus changes; host and automation events stay separate")

            focus("host-a")
            ok("/eval hl.plugin.test.set_mods(0, 1, 0, 0, 0)")
            before_target = len(target.sync())
            unchanged(lambda: (auto("key 30 down"), auto("key 30 up")))
            assert "key 30 1 97" in target.events[before_target:]
            ok("/eval hl.plugin.test.set_mods(0, 0, 0, 0, 0)")
            check("automation XKB modifiers are independent of host modifiers")

            layout_start = len(target.sync())
            ok("/eval hl.config({ input = { kb_layout = 'de' } })")
            host_key(21, True)
            host_key(21, False)
            unchanged(lambda: (auto("key 21 down"), auto("key 21 up")))
            assert "key 21 1 121" in target.events[layout_start:], target.events[layout_start:]
            assert "keymap" not in target.events[layout_start:], target.events[layout_start:]
            assert "key 21 1 122" in host_a.sync(), host_a.events
            auto("end")
            target.sync()
            assert "keymap" in target.events[layout_start:]
            ok("/eval hl.config({ input = { kb_layout = 'us' } })")
            auto("begin class:target")
            check("automation keymap stays stable across host layout changes and restores on end")

            auto("key 42 down")
            auto("key 30 down")
            auto("button 272 down")
            auto("move 40 50")
            for index in range(10):
                focus("host-a" if index % 2 else "host-b")
                assert status()["keys"] == [30, 42] and status()["buttons"] == [272]
            focus("target")
            visit_start = len(target.sync())
            host_key(48, True)
            host_key(48, False)
            host_key(30, True)
            host_key(30, False)
            host_move("target", 160, 170)
            host_click(273, True)
            host_click(273, False)
            ok("/eval hl.plugin.test.scroll(12)")
            ok("/eval hl.plugin.test.set_mods(0, 1, 0, 0, 0)")
            ok("/eval hl.plugin.test.set_mods(0, 0, 0, 0, 0)")
            visit = target.sync()[visit_start:]
            assert any(e.startswith("key 48 1 ") for e in visit), visit
            assert any(e.startswith("key 30 0 ") for e in visit), visit
            assert "button 273 1" in visit and "button 273 0" in visit, visit
            assert any(e.startswith("motion ") for e in visit), visit
            assert any(e.startswith("axis ") for e in visit), visit
            assert "modifiers 0 0 0 0" in visit, visit
            assert status()["keys"] == [30, 42] and status()["buttons"] == [272]
            assert status()["position"] == [40.0, 50.0]
            check("default coexistence forwards host input without overwriting automation state")

            injection_start = len(target.sync())
            unchanged(lambda: (auto("key 46 down"), auto("key 46 up")))
            unchanged(lambda: (auto("button 274 down"), auto("button 274 up")))
            injected = target.events[injection_start:]
            assert "key 46 1 67" in injected, injected
            assert "key 46 0 67" in injected, injected
            click = injected.index("button 274 1")
            assert injected[click - 1] == "motion 40.000 50.000", injected
            assert status()["keys"] == [30, 42] and status()["buttons"] == [272]
            check("injection reapplies automation modifiers and pointer position after host input")

            cursor_before = status()["host"]["cursor_updates"]
            target.request("cursor", "cursor-set")
            assert status()["host"]["cursor_updates"] == cursor_before + 1
            host_move("target", 150, 160)
            departure_start = len(target.sync())
            focus("host-a")
            assert "frame" in target.events[departure_start:], target.events[departure_start:]
            unchanged(lambda: target.request("cursor", "cursor-set"))
            focus("target")
            check("native cursor routing and final pointer frames preserve foreground input")

            unchanged(lambda: auto("block-input on"))
            assert status()["block_input"]
            blocked_start = len(target.sync())
            host_key(48, True)
            host_key(48, False)
            ok("/eval hl.plugin.test.set_mods(0, 1, 0, 0, 0)")
            ok("/eval hl.plugin.test.set_mods(0, 0, 0, 0, 0)")
            host_move("target", 180, 190)
            host_click(273, True)
            host_click(273, False)
            ok("/eval hl.plugin.test.scroll(12)")
            assert not [e for e in target.sync()[blocked_start:] if e != "sync"], target.events[blocked_start:]
            assert status()["keys"] == [30, 42] and status()["buttons"] == [272]
            assert status()["position"] == [40.0, 50.0]
            unchanged(lambda: (auto("key 46 down"), auto("key 46 up")))
            unchanged(lambda: (auto("button 274 down"), auto("button 274 up")))
            assert "key 46 1 67" in target.events[blocked_start:]
            assert "button 274 1" in target.events[blocked_start:]
            focus("host-a")
            other_start = len(host_a.sync())
            host_key(48, True)
            host_key(48, False)
            host_click(273, True)
            host_click(273, False)
            other = host_a.sync()[other_start:]
            assert any(e.startswith("key 48 1 ") for e in other), other
            assert "button 273 1" in other, other
            focus("target")
            assert "keyboard-leave" not in target.events[visit_start:]
            assert "pointer-leave" not in target.events[visit_start:]
            assert "keyboard-enter" not in target.events[visit_start:]
            check("block-input suppresses target host events, not automation or other clients")

            unchanged(lambda: auto("block-input off"))
            assert not status()["block_input"]
            unblocked_start = len(target.sync())
            host_key(48, True)
            host_key(48, False)
            host_move("target", 140, 150)
            host_click(273, True)
            host_click(273, False)
            ok("/eval hl.plugin.test.scroll(12)")
            unblocked = target.sync()[unblocked_start:]
            assert any(e.startswith("key 48 1 ") for e in unblocked), unblocked
            assert "button 273 1" in unblocked, unblocked
            assert any(e.startswith("motion ") for e in unblocked), unblocked
            assert any(e.startswith("axis ") for e in unblocked), unblocked
            assert status()["keys"] == [30, 42] and status()["buttons"] == [272]
            auto("key 30 up")
            auto("key 42 up")
            auto("button 272 up")
            auto("block-input on")
            auto("end")
            assert not status()["active"] and not status()["block_input"]
            check("unblocking resumes delivery without restarting; end resets the input policy")
            resume_start = len(target.sync())
            host_key(48, True)
            host_key(48, False)
            host_click(273, True)
            host_click(273, False)
            target.sync()
            assert any(e.startswith("key 48 1 ") for e in target.events[resume_start:])
            assert "button 273 1" in target.events[resume_start:]
            focus("host-a")
            auto("begin class:target")
            check("ending a blocked foreground session restores ordinary host routing")

            for request in ("key 30 up", "button 272 up", "key -1 down", "button 1 down", "move -1 10", "move 999999 10",
                            "block-input", "block-input yes", "block-input on extra", "screenshot", "screenshot one two"):
                assert command("/hyprauto " + request).startswith("error:"), request
            auto("key 30 down")
            assert command("/hyprauto key 30 down").startswith("error:")
            auto("button 272 down")
            cleanup_start = len(target.sync())
            unchanged(lambda: auto("end"))
            cleanup = target.events[cleanup_start:]
            assert any(e.startswith("key 30 0 ") for e in cleanup), cleanup
            assert "button 272 0" in cleanup and "keyboard-leave" in cleanup and "pointer-leave" in cleanup, cleanup
            assert not status()["active"]
            check("validation rejects unpaired input; ending releases held inputs and leaves")

            auto("begin class:target")
            auto("block-input on")
            auto("key 30 down")
            auto("button 272 down")
            unmap_start = len(target.sync())
            target.request("unmap", "unmapped")
            wait_for(lambda: not status()["active"], "unmap cancels session")
            assert not status()["block_input"]
            assert any(e.startswith("key 30 0 ") for e in target.events[unmap_start:]), target.events[unmap_start:]
            assert "button 272 0" in target.events[unmap_start:]
            check("target unmap cancels session and releases held input")

            target.close()
            clients.remove(target)
            wait_for(lambda: len(json.loads(command("j/clients"))) == 2, "old target disconnected")
            target = Client("target", env, logs)
            clients.append(target)
            wait_for(lambda: len(json.loads(command("j/clients"))) == 3, "new target mapped")
            focus("host-b")
            auto("begin class:target")
            auto("block-input on")
            auto("key 30 down")
            auto("button 272 down")
            target.close()
            clients.remove(target)
            wait_for(lambda: not status()["active"], "disconnect cancels session")
            assert not status()["block_input"]
            host_key(48, True)
            host_key(48, False)
            host_b.sync()
            assert compositor.poll() is None
            check("target disconnect leaves compositor and host input usable")

            target = Client("target", env, logs)
            clients.append(target)
            wait_for(lambda: len(json.loads(command("j/clients"))) == 3, "replacement target mapped")
            focus("host-a")
            auto("begin class:target")
            auto("block-input on")
            auto("key 30 down")
            auto("button 272 down")
            unload_start = len(target.sync())
            ok(f"/plugin unload {plugin}")
            sync_all()
            events = target.events[unload_start:]
            assert any(e.startswith("key 30 0 ") for e in events) and "button 272 0" in events, events
            ok(f"/plugin load {plugin}")
            focus("target")
            host_key(48, True)
            host_key(48, False)
            target.sync()
            assert any(e.startswith("key 48 1 ") for e in target.events)
            unchanged(lambda: auto("begin class:target"))
            assert status()["active"] and not status()["block_input"]
            foreground_start = len(target.sync())
            host_key(48, True)
            host_key(48, False)
            unchanged(lambda: (auto("key 30 down"), auto("key 30 up")))
            foreground = target.events[foreground_start:]
            assert any(e.startswith("key 48 1 ") for e in foreground), foreground
            assert "key 30 1 97" in foreground, foreground
            assert "keyboard-leave" not in foreground
            auto("end")
            check("unload/reload restores host routing and permits coexisting foreground sessions")
            print(f"All checks passed. Logs: {logs}", flush=True)
        finally:
            for client in clients:
                client.close()
            if compositor and compositor.poll() is None:
                compositor.terminate()
                try:
                    compositor.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    compositor.kill()
                    compositor.wait()
            for log in runtime.glob("hypr/*/hyprland.log"):
                shutil.copyfile(log, logs / "hyprland-internal.log")
            compositor_log.close()
            print(f"Test artifacts: {logs}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--plugin", type=Path)
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    plugin = args.plugin.resolve() if args.plugin else build_dir / "hyprauto.so"
    run(args.binary.resolve(), build_dir, plugin)
