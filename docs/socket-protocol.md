# Session socket protocol (version 1)

## Discovery and ownership

`hyprctl hyprauto status` returns JSON:

```json
{"socket":"/run/user/1000/hypr/<instance>/.hyprauto.sock","protocol":1,"sessions":0,"lease_ms":30000}
```

Connect to `socket` using an AF_UNIX stream socket. Its filesystem permissions are `0600`, and the plugin verifies the peer UID matches its own. The service accepts at most 64 connections. Each accepted connection owns exactly one session, initially without a target; no explicit create command is needed. Session IDs are allocated by the plugin and are valid for the current service lifetime. They are for inspection and administrative cleanup, not connection resumption or authentication tokens.

Requests are implicitly scoped to their connection's session. They cannot address another session. A connection loss, `end`, forced administrative cleanup or lease expiry destroys the session and releases its target and input. There is no session resumption. Target destruction only clears the binding; the connection and session remain usable.

## Framing

Every frame consists of:

1. A four-byte unsigned **big-endian** length, counting the type byte and payload but not the length itself.
2. A one-byte type: ASCII `J` for JSON or `B` for binary.
3. The payload.

Stream reads may split or combine frames arbitrarily. Clients must buffer partial reads and process complete frames. Requests must be JSON frames of at most 16384 bytes including the type byte. Malformed envelopes, invalid lengths or unsupported frame types disconnect the client and clean up its session.

The first server frame is a JSON greeting:

```json
{"id":0,"session_id":1,"lease_ms":30000,"protocol":1}
```

Clients then send JSON requests with a nonzero unsigned 64-bit request ID and a command string:

```json
{"id":1,"command":"set-target class:target"}
{"id":2,"command":"move 40 50"}
```

IDs should be unique for the connection lifetime. Heartbeats and operations can be outstanding at the same time. Commands start on the compositor event loop in receive order; timed clicks defer completion, so replies can arrive out of order. The client must match responses by ID.

A successful control response contains a string result:

```json
{"id":2,"result":"ok"}
```

Commands returning structured data encode that JSON as the result string. For example, a screenshot submission returns:

```json
{"id":3,"result":"{\"id\":1}"}
```

Command errors retain the connection and return an error string:

```json
{"id":4,"error":"coordinates must be inside the target surface"}
```

Successful `screenshot-read` responses are binary frames. The first eight payload bytes contain the request ID as an unsigned big-endian integer; remaining payload bytes are the requested pixels, without a `data:` prefix. A failed read uses a JSON error response as above.

Output is nonblocking and buffered. Clients must continue reading responses; a queued output backlog exceeding 1 MiB disconnects the client. Slow readers do not block the compositor.

## Commands

| Command | Result / behavior |
| --- | --- |
| `heartbeat` | `ok`; renew the lease without requiring a target. |
| `status` | JSON string with `session_id`, `target` (app ID), `active`, `block_input`, `keys`, `buttons`, `position` and `host` state. `active` means a target is bound, not merely that the session exists. |
| `set-target <selector>` | `ok`; select a mapped native Wayland window. One session per Wayland client. A failed switch preserves the old target and state. Selecting the current target is a no-op. A successful switch releases the old input and capture and resets the input policy. |
| `key <code> down\|up` | `ok`; press or release a Linux evdev key code. |
| `move <x> <y>` | `ok`; move the automation pointer within the main-surface bounds. |
| `button <code> down\|up` | `ok`; press or release a Linux evdev mouse button code. |
| `click <x> <y> <button-code> <hold-ms>` | Move, press and release an evdev mouse button; reply `ok` after release. |
| `click-key <key-code> <hold-ms>` | Press and release an evdev key; reply `ok` after release. |
| `block-input on\|off` | `ok`; block or allow host input to the bound client. |
| `screenshot [path]` | JSON string containing a capture `id`. An optional double-quoted path selects PNG output; omit it for retained raw pixels. |
| `screenshot-status <capture-id>` | JSON string with `state`: `pending`, `ready` or `failed`. A failed state includes `error`. A ready state includes `width`, `height`, `format`, `size` and `path`. |
| `screenshot-read <capture-id> <offset> <length>` | Binary frame; read 1–65536 bytes of ready raw pixels. |
| `screenshot-release <capture-id>` | `ok`; release or cancel the retained capture. |
| `end` | `ok`, then disconnect and destroy the session. |

All commands except `heartbeat`, `status`, `set-target` and `end` require a target. Duplicate presses, unmatched releases and invalid arguments are rejected. Command tokens are whitespace-separated; a selector is one token. Screenshot paths support double quoting and backslash escaping through C++ `std::quoted` syntax.

Click hold durations must be integers from 0 to 10000 ms. Zero releases immediately; positive durations are minimum holds and may finish later if the compositor is busy. A click rejects an already-held key or button without changing it. While a click is pending, only `status`, `heartbeat` and `end` are accepted; other commands return an error rather than being queued. Clients requiring ordered actions must wait for completion before sending the next action. Target loss or session cleanup cancels the pending click and releases held input; target loss leaves the connection usable. Successful replies indicate input was sent, not that the application acted on it.

Each successful command renews the 30-second lease, including `status`. Errors and incomplete frames do not renew it. Send `heartbeat` every five seconds even when idle or waiting for a screenshot; do not serialize heartbeats behind long-running application actions. The expiry timer runs every second, so cleanup occurs on the first timer tick after expiry. A client paused longer than the lease must create a new connection.

## Capture ownership

Capture IDs belong to the session, not to the service globally. Each session retains at most one capture, while different sessions may retain captures simultaneously. IDs increase within a session; do not reuse an ID after target changes or release.

Poll `screenshot-status` until ready before reading. Raw pixels are top-to-bottom, tightly packed, premultiplied BGRA, four bytes per pixel. Read exactly `size` bytes in bounded chunks. PNG captures have no retained raw pixels. A PNG write already in progress may finish after cancellation.

Switching or losing a target invalidates its capture. Ending one session does not cancel another session's capture. The GPU stage times out after ten seconds when polled; clients should also enforce their own request and capture deadlines.

## Administrative interface

Only these operations use hyprctl:

- `hyprauto status`: service discovery and health information.
- `hyprauto sessions`: JSON array of session status objects, including sessions without targets.
- `hyprauto end <session-id>`: forcibly disconnect an existing session; unknown IDs return an error.

The administrative interface is available to the same local user as Hyprland's normal IPC. On plugin unload, all sessions are closed and the socket is removed. Reload requires new connections.
