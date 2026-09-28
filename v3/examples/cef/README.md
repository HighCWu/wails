# CEF desktop smoke example

Build from `v3`:

```sh
go build -tags 'gtk3 wails_cef' -o /tmp/wails-cef-smoke ./examples/cef
WAILS_WEBVIEW_BACKEND=cef WAILS_CEF_DIR=/absolute/path/to/cef /tmp/wails-cef-smoke
```

The vendored headers target CEF **154.0.28 / Chromium 154.0.8037.58**, API
15400. Merge the minimal distribution's Release and Resources directories into
the runtime directory. API hash validation rejects incompatible libraries.
The example also supports `WAILS_WEBVIEW_BACKEND=system` and builds without
`wails_cef` using `-tags gtk3`.

On Linux GTK4, Windows and macOS, build with `-tags wails_cef` (without
`gtk3`). CEF replaces the webview inside Wails' native window. Linux uses an
X11 child window, Windows uses an HWND child, and macOS embeds an NSView.
The system backend remains available in the same executable.

For Windows, merge the matching windows64 minimal package's Release and
Resources directories. For macOS, keep the framework's bundle and symlinks
intact in the application's `Contents/Frameworks` directory. Set `WAILS_CEF_DIR`
to that directory and `WAILS_CEF_SUBPROCESS_PATH` to the executable inside the
base Helper application. Chromium 154 also requires the sibling `Helper
(Renderer).app` and `Helper (GPU).app` bundles with matching executable names.
`v3/scripts/prepare-cef.py` demonstrates the complete smoke application's
bundle layout and ad-hoc signing; shipping applications need their own bundle
identifiers, signing and distribution setup.

The page exercises asset loading, runtime readiness, keyboard input, fetch RPC,
Go-to-JavaScript execution (Ctrl+M), resize, and DevTools.

## Automated regression

Requires Python 3 + Pillow, Xvfb, openbox, xdotool, xwininfo, and ImageMagick
`import`:

```sh
python3 examples/cef/smoke.py /tmp/wails-cef-smoke \
  --runtime /absolute/path/to/cef --devtools --output /tmp/cef-smoke-results
python3 examples/cef/smoke.py /tmp/wails-cef-smoke \
  --backend system --output /tmp/system-smoke-results
```

The script allocates a private display and never sends input to the user's
DISPLAY. It checks changing page pixels against RPC logs, window titles and
geometry, then verifies a clean exit with DevTools still open. Logs, screenshots
and the native window tree are retained. A simple GTK input context isolates the
default test from the desktop input method daemon. Use `--ime` for the separate
composition test.

System mode also checks Ctrl+M/ExecJS. The script explicitly activates the
private window and holds Ctrl for 120 ms before M: upstream GTK's 50 ms key
debounce otherwise filters a shortcut generated with xdotool's default timing.

Extended CEF checks use fake camera/microphone devices, without automatically
granting permission, so allow/deny policies exercise the real permission handler:

```sh
python3 examples/cef/smoke.py /tmp/wails-cef-smoke \
  --runtime /absolute/path/to/cef --extended --devtools --output /tmp/cef-extended
python3 examples/cef/smoke.py /tmp/wails-cef-smoke \
  --runtime /absolute/path/to/cef --ime --output /tmp/cef-ime
```

Extended mode verifies frameless movement and edge resize, separate windows,
Unicode clipboard/RPC, native file drops with target details, rejection outside
targets and in disabled windows, and DevTools shutdown. It requires `xclip` and
system Python (`/usr/bin/python3`) with PyGObject/GTK3. IME mode additionally
requires IBus and `dbus-daemon`; it starts an isolated D-Bus/IBus session with a
deterministic engine that emits real Chinese preedit and commit events. This
checks Chromium's composition integration, not a particular input method's
vocabulary, candidate UI, or the user's Sogou/Fcitx configuration.

For **passive** rendering checks, `CEF_SMOKE_PASSIVE=1` makes the example's
windows reject keyboard focus, runs a delayed RPC that turns the page green,
and exits after 15 seconds. It sends no input. The explicit passive helper uses
only window queries and a screenshot of the example window:

```sh
python3 examples/cef/passive.py /tmp/wails-cef-smoke \
  --runtime /absolute/path/to/cef --output /tmp/cef-passive
```

It uses the selected `DISPLAY` and does not allocate a virtual display. A passive check on the NVIDIA
RTX 4080 desktop passed in addition to the private Xvfb checks.

CEF uses the X11 DefaultVisual (as in cefclient), an Alloy browser directly
embedded under the GTK drawing area, and a GTK-driven CEF message loop.
Frameless gestures follow pointer coordinates while Chromium owns its native
grab; window-manager snapping is not implemented by that movement path.
Hardware media devices remain outside these tests. GTK3 and GTK4 run the same
Linux interaction suite; GTK4 uses its own X11 container adapter.

## GitHub Actions

`.github/workflows/cef-test-v3.yml` runs on v3 pull requests, master pushes,
and manual dispatches. It follows the v3 workflows' checkout/setup-go,
Linux dependency installation, native runner matrix and artifact conventions.
The CEF job downloads the exact minimal distribution above, checks its published
checksum, and runs the extended, IME and system-backend regressions on private
Xvfb displays. Diagnostic artifacts exclude browser caches and runtime binaries.

Separate Windows, macOS and Linux jobs build the application and plain example
both with and without `wails_cef`, and test backend selection. Native Windows
(amd64) and macOS (arm64) jobs download the pinned runtime, build real CEF
applications, and drive their disposable desktops with `native_smoke.py`.
That script refuses to send input outside GitHub Actions. It checks presented
pixels, typing, RPC, Ctrl+M, native resize and shutdown with DevTools open.
Each native scenario runs in a separate process; `results.json` reports all
failures instead of allowing one failed feature to hide later checks. The suite
covers Chromium composition/preedit/commit (via CDP), native Unicode file drag
and drop, independent windows and shortcuts, permission allow/deny, actual
captured video frames and encoded audio bytes, native top-level/attached dialogs,
whole-window mouse passthrough and opaque frameless dragging. CDP composition
checks do **not** validate the Windows TSF or macOS input-method frontend;
the Linux IBus test remains the native OS IME regression.

For real camera/microphone testing, manually dispatch this workflow and select
`hardware_platform: windows64` or `macosarm64`. That platform's job then requires
a **dedicated, interactive** self-hosted runner labelled `cef-hardware` plus
`Windows` + `X64` or `macOS` + `ARM64`, with capture devices and OS privacy permissions configured.
It does not append fake-device switches and fails when capture produces no
video frame or encoded audio. Use an isolated test desktop: the suite sends real
input. No such hardware runner is provisioned by this repository, and a normal
hosted-runner pass must not be reported as physical-device validation.

### Capability boundaries

- Official CEF distributions target Windows, macOS and Linux; iOS and Android
  keep the system backend. The CEF Go/cgo files are excluded for both mobile
  targets, even when `wails_cef` is supplied. Forced CEF reports unavailable.
  See [CEF General Usage](https://chromiumembedded.github.io/cef/general_usage).
- CEF 154's native windowed renderer does not provide per-pixel transparent
  painting: `cef_settings_t.background_color` documents an opaque fallback.
  Per-pixel transparency and alpha-based click-through require an OSR renderer
  and native hit testing; this backend does not implement them. Whole-window
  `IgnoreMouseEvents` and dragging opaque content are separate regression cases,
  **not** evidence that transparent pixels automatically pass through clicks.
- Upstream has application/window/dialog unit tests, a Cocoa modal-loop test,
  and manual dialog/window examples. Its normal v3 CI builds examples and runs
  Go/JS tests; it does not drive the entire feature set above in a real webview.


Headless Linux runners select ANGLE SwiftShader explicitly; they do not use
`disable-gpu`. The system-WebKit regression disables its bubblewrap sandbox
only in that CI step because hosted runners restrict network namespaces.
Neither setting changes the backend's production defaults. The existing
upstream workflows remain responsible for their broader test suites.
