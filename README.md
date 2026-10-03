# hyprauto

A Hyprland plugin for sending keyboard and pointer input to native Wayland toplevels. Multiple automation sessions can control different clients simultaneously, independently of host focus and without moving the host cursor.

Each session has its own target, key/button state, modifiers, keyboard layout, pointer coordinates, input-blocking policy and screenshot job. A session can select one target at a time, and each Wayland client can belong to only one session.

> This plugin runs inside the compositor and uses private Hyprland hooks. Build it against the headers for the running Hyprland revision and test it in an isolated instance first. Application compatibility is not guaranteed.

## Installation

Install and enable the plugin with [hyprpm](https://wiki.hypr.land/Plugins/Using-Plugins/):

```sh
hyprpm add https://github.com/sitiyou/hyprauto.git
hyprpm enable hyprauto
```

After upgrading Hyprland, run `hyprpm update` to rebuild plugins against matching headers.

The Python controller requires NumPy and the `hyprauto_ctrl` package available on Python's import path. The plugin provides its own Unix socket; no external service, helper executable or subprocess is required for automation.

## Usage

Use one controller for each automation session:

```python
from hyprauto_ctrl import HyprAutoController

with HyprAutoController() as first, HyprAutoController() as second:
    for controller, target in ((first, "class:target-a"), (second, "class:target-b")):
        job = controller.post_connection(target).wait()
        if not job.succeeded:
            raise RuntimeError(job.error)

    first.post_click(40, 50).wait()
    second.post_click_key(30).wait()
    image = first.post_screencap().wait().get()
    print(first.session_id, second.session_id)
```

Pass `instance="instance-signature"` at construction when selecting a particular Hyprland instance. Otherwise the controller uses `HYPRLAND_INSTANCE_SIGNATURE`. The instance's runtime directory is resolved under `XDG_RUNTIME_DIR`.

`post_connection()` creates a session, optionally selecting a target. `set_target(selector)` switches only that controller's target. Select a mapped native Wayland window with bound keyboard and pointer resources, using selectors such as `class:target` or `address:0x...`. Selecting an unavailable or occupied target fails without changing the existing target or held input. Selecting the current target again does not reset it.

Jobs expose `succeeded` and `error`; always check them when handling failures. Jobs execute in submission order within each controller; separate controllers remain independent. Click jobs finish after release is sent, not after the application responds.

`post_click_key(key, hold_ms=80)` uses Linux evdev key codes and holds the key for at least 80 ms by default. For example, `42` is left Shift and `30` is A. `post_click(x, y, contact=0, pressure=1, hold_ms=50)` holds the mouse button for at least 50 ms by default. Mouse contacts `0`, `1` and `2` mean left, right and middle button. Coordinates are relative to the target's main surface and must remain inside it.

Both click methods accept integer hold durations from 0 to 10000 ms; zero requests immediate release and may be missed by applications that sample input per frame. Clicking an already-held key or button fails without releasing it. For longer holds, use explicit down/up methods.

### Cleanup and connection failures

Use a context manager or call `close()` to release input and end your session. This does not affect other sessions or unload the plugin.

A lost socket connection immediately releases that session's held input and screenshots. A 30-second lease also cleans up clients that remain connected but stop responding. Python automatically sends a heartbeat every five seconds, including while idle. Debugging pauses or process suspension exceeding the lease can end the session.

Closing or unmapping a target releases its input and invalidates its screenshot, but leaves the session connected so it can select another target. Plugin unload closes all sessions and removes its socket. Connection failures are reported explicitly: the controller does not automatically reconnect or replay input. Call `post_connection()` again to create a new session.

### User and automation input

By default, user input and automation input can coexist. Connecting or taking screenshots does not change the target's input focus. Host focus changes do not interrupt automation, and user input follows the focused window, including another window of the same application. Before injecting a key or button, automation reapplies its own modifiers or pointer position.

To block user input to a session's target:

```python
controller.post_block_input(True).wait()
controller.post_block_input(False).wait()
```

Blocking filters user keyboard and pointer events sent to that Wayland client. It does not affect automation, other clients, host focus, physical cursor movement, foreground cursor appearance or Hyprland key bindings. The policy resets when the target binding ends.

User and automation events share the target client's protocol resources; application-level state isolation is not guaranteed. Overlapping presses, releases and interleaved events may affect the application. Toggling the block policy does not replay physical input state; release held user keys and buttons before changing it.

### Screenshots

`post_screencap()` returns a NumPy BGR image directly, without PNG encoding, decoding or temporary files. The latest image is available from `controller.cached_image`.

Each session can retain one capture at a time; different sessions can capture independently. Screenshots require a mapped target and Hyprland's OpenGL renderer. Targets with `noscreenshare` enabled are rejected. The image is confined to the main-surface bounds, not the monitor or host cursor; subsurfaces and popups within those bounds may be included. Hyprland window opacity, inactive-window dimming and decorations are excluded. Client-surface alpha is preserved by the socket protocol and removed by the Python controller.

## Management with hyprctl

```sh
hyprctl hyprauto status
hyprctl hyprauto sessions
hyprctl hyprauto end <session-id>
```

`status` reports the socket address, protocol version, session count and lease duration. `sessions` lists session IDs, targets and input state. `end <session-id>` forcibly disconnects that session and releases its input.

Target selection, input, screenshots and normal session cleanup use the dedicated socket, not hyprctl. For other integrations, see the [socket protocol](docs/socket-protocol.md).

When using an isolated instance, pass `hyprctl --instance "$INSTANCE"` and use that instance's `XDG_RUNTIME_DIR`.

## Uninstallation

```sh
hyprpm disable hyprauto
hyprpm remove https://github.com/sitiyou/hyprauto.git
```

## Scope

- Multiple sessions for different native Wayland clients; one target per session and one session per client. At most 64 connections are accepted.
- Automation remains on the selected target while user input follows host focus. Keyboard and pointer rebinding is supported during a session; application-level input state isolation is not guaranteed.
- The socket is local and restricted to the user running Hyprland. Cross-user and network access are not supported.
- XWayland, IME, popup/subsurface hit-testing, pointer-lock/relative-pointer, touch/tablet, scroll injection and drag-and-drop are not supported.
- Protocol behavior is tested with the included client. Compatibility with GTK, Qt, browsers and games requires separate testing.

