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
test from the desktop input method daemon; this does not validate IME support.

System mode checks rendering, input, RPC, resize and shutdown. Ctrl+M/ExecJS is
explicitly skipped there because the shortcut did not fire in the test
environment; it is asserted in CEF mode. Real NVIDIA/XFCE presentation, IME,
frameless dragging, media permissions and file drops need separate validation.

CEF uses the X11 DefaultVisual (as in cefclient), an Alloy browser directly
embedded under the GTK drawing area, and a GTK-driven CEF message loop.
