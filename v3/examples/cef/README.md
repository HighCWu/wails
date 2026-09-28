# CEF GTK3 smoke example

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
Hardware media devices and GTK4 remain outside these tests.
