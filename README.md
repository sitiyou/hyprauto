# hyprauto

A window automation tool for Hyprland. Control native Wayland windows with keyboard and pointer input while keeping your desktop focus and physical cursor usable. Run independent automation sessions on multiple windows at once, and capture target windows for visual feedback.

Hyprauto is delivered as a Hyprland plugin with a Python controller. It uses a dedicated local Unix socket; automation does not require an external service or helper executable.

## Why hyprauto

- **Automate a chosen window without taking over your desktop.** Automation stays on its selected target while host focus changes and physical input continues to follow the focused window.
- **Run multiple windows in parallel.** Each controller has its own session, target, input state and capture.
- **Build visual automation loops.** Capture the target window, inspect the result, then submit the next action.

## Installation

Install and enable the plugin with [hyprpm](https://wiki.hypr.land/Plugins/Using-Plugins/):

```sh
hyprpm add https://github.com/sitiyou/hyprauto.git
hyprpm enable hyprauto
```

After upgrading Hyprland, run `hyprpm update` to rebuild plugins against matching headers. Hyprauto uses private Hyprland APIs; build and test it against the Hyprland version you run, preferably in an isolated instance.

The Python controller requires NumPy and the `hyprauto_ctrl` package available on Python's import path. The controller uses `HYPRLAND_INSTANCE_SIGNATURE` and `XDG_RUNTIME_DIR` to find the running Hyprland instance by default.

## Quick start

Use one controller per automation session. This example connects to two windows, sends input to both, and captures one target:

```python
from hyprauto_ctrl import HyprAutoController

with HyprAutoController() as first, HyprAutoController() as second:
    for controller, target in ((first, "class:target-a"), (second, "class:target-b")):
        job = controller.post_connection(target).wait()
        if not job.succeeded:
            raise RuntimeError(job.error)

    first.post_click(40, 50).wait()
    first.post_touch_down(40, 50).wait()
    first.post_touch_move(60, 70).wait()
    first.post_touch_up().wait()
    second.post_click_key(30).wait()
    image = first.post_screencap().wait().get()
```

Pass `instance="instance-signature"` to `HyprAutoController` to select a particular Hyprland instance instead of using the environment variables.

## Targets and input

`post_connection()` creates a session and can select its initial target. `set_target(selector)` switches the target for that session. Select a mapped native Wayland window with bound keyboard and pointer resources, using selectors such as `class:target` or `address:0x...`. A Wayland client can belong to only one session. Failed target changes preserve the current target and held input; selecting the current target again is a no-op.

`post_click(x, y, contact=0, pressure=1, hold_ms=50)` clicks relative to the target's main surface. Contacts `0`, `1` and `2` mean left, right and middle mouse button. `post_click_key(key, hold_ms=80)` uses Linux evdev key codes; for example, `42` is left Shift and `30` is A. Click durations are integers from 0 to 10000 ms. Zero requests immediate release and may be missed by applications that sample input per frame.

For held input, use `post_key_down()` and `post_key_up()`. Jobs expose `succeeded` and `error`; check them when handling failures. Actions are submitted in order within a controller, while separate controllers operate independently. A successful job means input was sent, not that the application acted on it.

By default, user input and automation input can coexist. Connecting or taking a screenshot does not change host focus. Before injecting a key or button, automation reapplies its own modifiers or pointer position. To block user keyboard and pointer input to the session's target, use `post_block_input(True)` and restore it with `post_block_input(False)`. This filter does not affect other clients, physical cursor movement or Hyprland key bindings.

User and automation events share the target client's protocol resources, so application-level input state isolation is not guaranteed. Overlapping presses, releases and interleaved events may affect the application. Changing the input-blocking policy does not replay physical input state; release held user keys and buttons before changing it.

## Screenshots

`post_screencap()` captures the selected window and returns a NumPy BGR image. The latest image is also available as `controller.cached_image`.

Screenshots require a mapped target and Hyprland's OpenGL renderer. Targets with `noscreenshare` enabled are rejected. Each session retains at most one capture at a time. Captures are confined to the main-surface bounds, not the monitor or host cursor; subsurfaces and popups within those bounds may be included. Window opacity, inactive-window dimming and decorations are excluded.

## Sessions and management

Use a context manager or call `close()` to release input and end a session. Closing or unmapping a target releases its input and invalidates its screenshot, while leaving the session connected so it can select another target. A lost socket connection, plugin unload or 30-second lease expiry cleans up the session. The Python controller sends a heartbeat every five seconds; a paused process can outlast the lease. Connections are not resumed automatically. The local socket is restricted to the user running Hyprland and accepts up to 64 connections.

Use `hyprctl` for service discovery, session inspection and forced cleanup:

```sh
hyprctl hyprauto status
hyprctl hyprauto sessions
hyprctl hyprauto end <session-id>
```

Target selection, input, screenshots and normal session cleanup use the dedicated socket. For integrations outside Python, see the [socket protocol](docs/socket-protocol.md).

## Scope

Hyprauto targets native Wayland windows on Hyprland. XWayland windows are not supported for automation or screenshots; IME, popup/subsurface hit-testing, pointer-lock/relative-pointer, native touch/tablet input, scroll injection and drag-and-drop are also unsupported. Compatibility with individual applications, including GTK, Qt, browsers and games, requires separate testing.

## Uninstallation

```sh
hyprpm disable hyprauto
hyprpm remove https://github.com/sitiyou/hyprauto.git
```
