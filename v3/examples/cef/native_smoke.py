#!/usr/bin/env python3
"""Native CEF interaction test for disposable Windows/macOS CI desktops."""
import argparse
from collections import Counter
import os
from pathlib import Path
import signal
import subprocess
import time
import sys
import shutil
import re
import json
import socket
import urllib.request
import websocket
import pyautogui as ui

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("binary", type=Path)
p.add_argument(
    "--scenario",
    choices=[
        "all",
        "core",
        "composition",
        "media",
        "crash",
        "dialogs",
        "file-dialogs",
        "multiwindow",
        "mouse",
        "drop",
        "upstream",
    ],
    default="all",
)
p.add_argument("--suite", action="store_true")
p.add_argument("--exclude", default="",
               help="Comma-separated scenario names skipped by --suite")
p.add_argument("--runtime", required=True, type=Path)
p.add_argument("--output", required=True, type=Path)
p.add_argument("--helper", type=Path)
p.add_argument(
    "--hardware-media",
    action="store_true",
    default=os.environ.get("CEF_HARDWARE_MEDIA") == "true",
    help="Require real capture devices; never append fake-device switches",
)
a = p.parse_args()
if os.environ.get("GITHUB_ACTIONS") != "true":
    raise SystemExit(
        "Native input automation is restricted to disposable GitHub Actions desktops"
    )
a.output.mkdir(parents=True, exist_ok=True)
if a.suite:
    results = {}
    # file-dialogs runs LAST: its prompt/panel dance is the one flow
    # that can leave the mac input pipeline wedged, and this order keeps
    # any fallout contained to itself.
    excluded = [s.strip() for s in a.exclude.split(",") if s.strip()]
    for scenario in [
        "core",
        "composition",
        "media",
        "crash",
        "dialogs",
        "multiwindow",
        "mouse",
        "drop",
        "upstream",
        "file-dialogs",
    ]:
        command = [
            sys.executable,
            __file__,
            str(a.binary),
            "--runtime",
            str(a.runtime),
            "--output",
            str(a.output / scenario),
            "--scenario",
            scenario,
        ]
        if scenario in excluded:
            continue
        if a.helper:
            command += ["--helper", str(a.helper)]
        if a.hardware_media:
            command += ["--hardware-media"]
        # One clean retry per scenario: hosted-runner input automation has
        # one-off focus/click races (a missed button, a first-run prompt),
        # and a fresh app process resolves them. Real defects fail both
        # attempts; retries are recorded, never hidden.
        verdict = "failed"
        for attempt in (1, 2):
            try:
                result = subprocess.run(command, timeout=300)
                if result.returncode == 0:
                    verdict = "passed" if attempt == 1 else "passed (retry)"
                    break
                verdict = "failed"
            except subprocess.TimeoutExpired:
                verdict = "timed out"
            if attempt == 1:
                print(f"RETRY {scenario} after {verdict}", flush=True)
            # A failed attempt leaves its app (and any open native dialog)
            # running, which would steal the retry's input. Kill orphans
            # by executable name.
            exe = str(a.binary)
            if sys.platform == "win32":
                subprocess.run(["taskkill", "/F", "/IM", os.path.basename(exe)],
                               capture_output=True)
            else:
                subprocess.run(["pkill", "-9", "-f", exe], capture_output=True)
            time.sleep(1)
        results[scenario] = verdict
    (a.output / "results.json").write_text(json.dumps(results, indent=2))
    print(json.dumps(results, indent=2), flush=True)
    raise SystemExit(0 if all(v == "passed" for v in results.values()) else 1)

env = dict(
    os.environ,
    WAILS_WEBVIEW_BACKEND="cef",
    WAILS_CEF_DIR=str(a.runtime.resolve()),
    WAILS_CEF_LOG_TO_FILE="1",
    CEF_SMOKE_DELAY_QUIT="1",
    CEF_SMOKE_TRACE_CRASH="1",
)
file_dir = a.output.parent / "file-dialogs-fixture" if a.suite else a.output / "file-dialogs-fixture"
if a.scenario == "file-dialogs":
    env["CEF_SMOKE_FILE_DIR"] = str(file_dir)
if a.scenario == "upstream":
    env["CEF_SMOKE_UPSTREAM"] = "1"
if a.helper:
    env["WAILS_CEF_SUBPROCESS_PATH"] = str(a.helper.resolve())
if a.hardware_media:
    if "fake" in env.get("WAILS_CEF_SWITCHES", ""):
        raise SystemExit(
            "Hardware media tests must not use fake devices or permission bypasses"
        )
else:
    env["WAILS_CEF_SWITCHES"] = "use-fake-device-for-media-stream"
# CDP covers Chromium's composition/preedit/commit path. This is deliberately
# distinct from the native OS IME frontend exercised by the Linux IBus suite.
with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    debug_port = probe.getsockname()[1]
env["WAILS_CEF_SWITCHES"] = (
    env.get("WAILS_CEF_SWITCHES", "") + f",remote-debugging-port={debug_port}"
)
if a.scenario == "media":
    env["WAILS_CEF_SWITCHES"] += ",enable-logging=stderr,v=1"
    env["WAILS_CEF_LOG_VERBOSE"] = "1"
log = a.output / "application.log"
source = None
bundle_id = ""


def activate_mac_app():
    # A dismissed prompt or a stray desktop click can leave Finder
    # frontmost; keystrokes would go nowhere until the app is activated
    # again.
    if sys.platform != "darwin":
        return
    import AppKit

    native_app = (
        AppKit.NSRunningApplication.runningApplicationWithProcessIdentifier_(
            app.pid
        )
    )
    native_app.activateWithOptions_(
        AppKit.NSApplicationActivateIgnoringOtherApps
    )
    time.sleep(0.5)


def dismiss_mac_alerts():
    # Fresh runner accounts raise a "Do you want to enable Dictation?"
    # prompt the first time a text field gets focus after the microphone
    # permission was granted. It swallows typing and shortcuts, and macOS
    # re-raises it at every later focus until it is answered. Accepting
    # (OK) correlated with the synthetic keyboard pipeline going dead for
    # the rest of the suite, so prefer Not Now here and lean on the
    # per-case verification + scenario retry to ride out repeats. Try
    # through System Events; if scripting is unavailable, click the
    # prompt's fixed on-screen spot — with no prompt present that point
    # is empty app content, which is why there is deliberately NO Escape
    # fallback (it would cancel a just opened chooser).
    script = '''
    tell application "System Events"
        repeat with pname in {"UserNotificationCenter", "CoreServicesUIAgent", "universalctrl", "NotificationCenter", "TextInputMenuAgent"}
            try
                tell application process (pname as text)
                    repeat with w in windows
                        try
                            if exists button "Not Now" of w then
                                click button "Not Now" of w
                                return "clicked"
                            end if
                        end try
                    end repeat
                end tell
            end try
        end repeat
        repeat with p in application processes
            try
                repeat with w in (every window of p)
                    try
                        if exists button "Not Now" of w then
                            click button "Not Now" of w
                            return "clicked"
                        end if
                    end try
                end repeat
            end try
        end repeat
    end tell
    return "none"
    '''
    result = subprocess.run(
        ["osascript", "-e", script], capture_output=True,
        text=True, timeout=20)
    if result.stderr.strip():
        print("dismiss_mac_alerts: " + result.stderr.strip(), flush=True)
    # Always finish with a click at the prompt's fixed spot: on the alert
    # it is the OK button, and with no alert up it is empty app content.
    # Callers re-type afterwards, so a stray click is harmless either way.
    width, height = ui.size()
    ui.click(int(width * 0.545), int(height * 0.330))
    time.sleep(0.5)
    return True


def origin():
    image = screenshot("active")
    pixels = image.load()
    points = [
        (x, y)
        for y in range(image.height)
        for x in range(image.width)
        if pixels[x, y] == (30, 102, 245)
    ]
    assert points, "Missing browser header"
    scale = image.width / ui.size().width
    return min(x for x, y in points) / scale, min(y for x, y in points) / scale


def control(role, name):
    wait_log(role + ":controls:")
    matches = re.findall(
        re.escape(role) + r":controls:(\{[^\n]+\})",
        log.read_text(encoding="utf-8", errors="replace"),
    )
    return json.loads(matches[-1])[name]


def window_position(title):
    if sys.platform == "darwin":
        import Quartz

        windows = Quartz.CGWindowListCopyWindowInfo(
            Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID
        )
        for w in windows:
            bounds = w[Quartz.kCGWindowBounds]
            # Upstream AppKit intentionally omits titles on frameless windows.
            overlay = (
                title == "CEF smoke overlay ready"
                and w.get(Quartz.kCGWindowOwnerPID) == app.pid
                and 390 <= bounds["Width"] <= 410
                and 250 <= bounds["Height"] <= 300
            )
            if w.get(Quartz.kCGWindowName) == title or overlay:
                return bounds["X"], bounds["Y"]
    else:
        windows = ui.getWindowsWithTitle(title)
        if windows:
            return windows[0].left, windows[0].top
    raise AssertionError("Missing native window " + title)


def wait_log(text, timeout=40):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        content = log.read_text(encoding="utf-8", errors="replace")
        failures = [line for line in content.splitlines() if "upstream:error:" in line]
        if failures:
            raise AssertionError("\n".join(failures))
        if text in content:
            return
        if app.poll() is not None:
            raise AssertionError(f"Application exited: {app.returncode}")
        time.sleep(0.2)
    raise AssertionError("Missing log marker: " + text)


def screenshot(name):
    image = ui.screenshot().convert("RGB")
    image.save(a.output / (name + ".png"))
    return image


with log.open("w") as output:
    if a.scenario == "file-dialogs":
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "file_dialog_checks",
            Path(__file__).with_name("file_dialog_checks.py"))
        fdm = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fdm)
        fdm.prepare_files(file_dir)
    app = subprocess.Popen(
        [str(a.binary.resolve())], env=env, stdout=output, stderr=subprocess.STDOUT
    )
    try:
        wait_log("main:ready")
        if sys.platform == "darwin":
            # A first-run system prompt left up by an earlier scenario
            # covers the app and fails the pixel check below.
            dismiss_mac_alerts()
            activate_mac_app()
        for _ in range(60):
            image = screenshot("initial")
            if Counter(image.getdata())[(30, 102, 245)] > 50000:
                break
            time.sleep(0.5)
        else:
            raise AssertionError("CEF content was not presented")
        pixels = image.load()
        points = [
            (x, y)
            for y in range(image.height)
            for x in range(image.width)
            if pixels[x, y] == (30, 102, 245)
        ]
        scale = image.width / ui.size().width
        left = min(x for x, y in points) / scale
        top = min(y for x, y in points) / scale

        def click(x, y):
            ui.click(left + x, top + y)
            time.sleep(0.3)

        if sys.platform == "darwin":
            import AppKit

            native_app = (
                AppKit.NSRunningApplication.runningApplicationWithProcessIdentifier_(
                    app.pid
                )
            )
            native_app.activateWithOptions_(
                AppKit.NSApplicationActivateIgnoringOtherApps
            )
            time.sleep(0.5)
            # The app's bundle domain, for NSOpenPanel location defaults.
            if a.helper:
                plist = Path(a.helper).parent.parent.parent / "Info.plist"
            else:
                plist = Path(a.binary).parent / "Info.plist"
            if plist.exists():
                import plistlib

                bundle_id = plistlib.loads(plist.read_bytes()).get(
                    "CFBundleIdentifier", "")
        # macOS: a preceding scenario can leave focus elsewhere; the first
        # click may only activate the window. Retry the type-and-greet
        # sequence, re-activating the app, before failing.
        greet_attempts = 3 if sys.platform == "darwin" else 1
        for attempt in range(greet_attempts):
            click(100, 164)
            if sys.platform == "darwin":
                # Same truncation workaround as type_text: confirm the
                # pasteboard before trusting cmd+v, otherwise type.
                subprocess.run(["pbcopy"], input=b"CEF154", check=True)
                board = subprocess.run(["pbpaste"], capture_output=True,
                                       text=True).stdout
                if board == "CEF154":
                    ui.hotkey("command", "a")
                    ui.hotkey("command", "v")
                else:
                    print("greet: pasteboard mismatch, typing", flush=True)
                    ui.write("CEF154", interval=0.1)
            else:
                ui.write("CEF154", interval=0.1)
            click(290, 164)
            try:
                wait_log('CEF_SMOKE_GREET "CEF154"', timeout=8)
                break
            except AssertionError:
                if attempt + 1 >= greet_attempts:
                    raise
                if sys.platform == "darwin":
                    import AppKit

                    native_app = AppKit.NSRunningApplication.runningApplicationWithProcessIdentifier_(
                        app.pid)
                    native_app.activateWithOptions_(
                        AppKit.NSApplicationActivateIgnoringOtherApps)
                    time.sleep(0.8)
        time.sleep(1)
        assert Counter(screenshot("rpc").getdata())[(0, 255, 0)] > 200000
        ui.keyDown("ctrl")
        time.sleep(0.15)
        ui.press("m")
        ui.keyUp("ctrl")
        wait_log("CEF_SMOKE_EXECJS main")
        if a.scenario in ("all", "crash"):
            # SIGKILL the renderer subprocess — the abnormal termination
            # real deployments hit. The backend must emit WindowRenderCrash
            # and schedule exactly one auto-reload (the cool-down prevents
            # reload loops) that restores a working page and RPC bridge.
            def renderer_pids():
                if sys.platform == "win32":
                    listing = subprocess.run(
                        ["powershell", "-NoProfile", "-Command",
                         f"Get-CimInstance Win32_Process -Filter "
                         f"\"ParentProcessId={app.pid}\" | "
                         "Where-Object { $_.CommandLine -match "
                         "'--type=renderer' } | Select-Object -ExpandProperty "
                         "ProcessId"],
                        capture_output=True, text=True, timeout=30).stdout
                    return [int(line) for line in listing.split()
                            if line.strip().isdigit()]
                pids = []
                for child in subprocess.run(
                        ["pgrep", "-P", str(app.pid)],
                        capture_output=True, text=True).stdout.split():
                    if sys.platform == "darwin":
                        args = subprocess.run(["ps", "-o", "command=", "-p", child],
                                              capture_output=True,
                                              text=True).stdout
                    else:
                        try:
                            args = Path(f"/proc/{child}/cmdline").read_bytes().decode(
                                errors="replace")
                        except OSError:
                            continue
                    if "--type=renderer" in args:
                        pids.append(int(child))
                return pids

            killed = renderer_pids()
            assert killed, "no CEF renderer subprocess found"
            for pid in killed:
                if sys.platform == "win32":
                    subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                                   capture_output=True)
                else:
                    os.kill(pid, signal.SIGKILL)
            wait_log("CEF_SMOKE_CRASH_EVENT main")
            fresh = []
            for _ in range(60):
                fresh = [pid for pid in renderer_pids() if pid not in killed]
                if fresh:
                    break
                time.sleep(0.5)
            assert fresh, "auto-reload did not spawn a replacement renderer"
            # The window title survives the renderer's death, so gate on
            # the reloaded runtime reporting ready again before typing.
            ready_deadline = time.monotonic() + 30
            while time.monotonic() < ready_deadline:
                if log.read_text(encoding="utf-8", errors="replace").count(
                        "main:ready") >= 2:
                    break
                time.sleep(0.2)
            assert log.read_text(encoding="utf-8", errors="replace").count(
                "main:ready") >= 2, "reloaded page never became ready"
            click(*control("main", "name"))
            ui.hotkey("command" if sys.platform == "darwin" else "ctrl", "a")
            ui.press("backspace")
            ui.write("CEF154", interval=0.1)
            click(*control("main", "greet"))
            for _ in range(100):
                if log.read_text(encoding="utf-8", errors="replace").count(
                        'CEF_SMOKE_GREET "CEF154"') >= 2:
                    break
                time.sleep(0.2)
            assert log.read_text(encoding="utf-8", errors="replace").count(
                'CEF_SMOKE_GREET "CEF154"') >= 2, "RPC did not recover after the crash"
            screenshot("crash-recovered")
            print(
                "PASS: renderer crash event, single auto-reload and recovered RPC",
                flush=True,
            )
        if a.scenario in ("all", "composition"):
            click(*control("main", "name"))
            ui.hotkey("command" if sys.platform == "darwin" else "ctrl", "a")
            ui.press("backspace")
            with urllib.request.urlopen(
                f"http://127.0.0.1:{debug_port}/json/list", timeout=10
            ) as response:
                pages = json.load(response)
            page = next(
                p
                for p in pages
                if p.get("type") == "page" and "role=main" in p.get("url", "")
            )
            ws = websocket.create_connection(
                page["webSocketDebuggerUrl"], suppress_origin=True, timeout=10
            )
            message_id = 0

            def cdp(method, params):
                global message_id
                message_id += 1
                ws.send(json.dumps(dict(id=message_id, method=method, params=params)))
                while True:
                    response = json.loads(ws.recv())
                    if response.get("id") == message_id:
                        assert "error" not in response, response
                        return response.get("result")

            try:
                cdp(
                    "Input.imeSetComposition",
                    dict(text="中文输入", selectionStart=0, selectionEnd=4),
                )
                wait_log("main:compositionupdate:中文输入")
                screenshot("composition")
                cdp("Input.insertText", dict(text="中文输入"))
                wait_log("main:compositionend:中文输入")
                click(*control("main", "greet"))
                wait_log('CEF_SMOKE_GREET "中文输入"')
            finally:
                ws.close()
            print(
                "PASS: Chromium composition/preedit/commit and Unicode RPC (CDP, not native OS IME)",
                flush=True,
            )
        click(260, 223)
        # Wails SetSize is the outer window size on Windows. Account for native
        # frame decorations instead of requiring a 1000x700 browser viewport.
        wait_log("main:size:")
        sizes = re.findall(
            r"main:size:(\d+)x(\d+)", log.read_text(encoding="utf-8", errors="replace")
        )
        assert any(
            950 <= int(w) <= 1000 and 650 <= int(h) <= 700 for w, h in sizes
        ), sizes
        screenshot("resized")
        if a.scenario == "upstream":
            ui.press("f8")
            wait_log("CEF_SMOKE_WINDOW_API requested", timeout=5)
            wait_log("main:upstream:window-api:passed", timeout=90)
            assert "upstream:error:" not in log.read_text(encoding="utf-8", errors="replace")
            screenshot("window-api")
            left, top = origin()
            print("PASS: upstream window-api maximise, toggle, fullscreen and restore", flush=True)
        if a.scenario in ("all", "media"):
            click(*control("main", "media"))
            wait_log("main:media:allowed:audio,video")
            wait_log("main:media:frame:")
            wait_log("main:media:audio-bytes:")
            print(
                "PASS: media permissions and captured video frame ("
                + ("hardware" if a.hardware_media else "fake devices")
                + ")",
                flush=True,
            )
        if a.scenario in ("all", "dialogs", "upstream"):
            for name, attached in [("dialog", "false"), ("attached", "true")]:
                click(*control("main", name))
                wait_log("CEF_SMOKE_DIALOG opened attached=" + attached)
                time.sleep(1)
                screenshot(name)
                # The main-thread callback must arrive while the native modal
                # dialog is still open, mirroring mainthread_darwin_test.go.
                wait_log("CEF_SMOKE_DIALOG dispatch attached=" + attached, timeout=2)
                if sys.platform == "win32":
                    import ctypes

                    dialogs = ui.getWindowsWithTitle("CEF smoke dialog")
                    assert dialogs, "Native dialog did not appear"
                    dialog = dialogs[0]
                    dialog.activate()
                    # SetForegroundWindow alone is not evidence that input
                    # reached the dialog. Use a real activation click and
                    # verify its HWND before sending the confirmation key.
                    ui.click(dialog.left + dialog.width / 2, dialog.top + 12)
                    time.sleep(0.3)
                    get_foreground = ctypes.windll.user32.GetForegroundWindow
                    get_foreground.restype = ctypes.c_void_p
                    foreground = get_foreground()
                    (a.output / (name + "-focus.json")).write_text(
                        json.dumps({"dialog": dialog._hWnd, "foreground": foreground})
                    )
                    assert foreground == dialog._hWnd, "Native dialog lacks keyboard focus"
                ui.keyDown("enter")
                time.sleep(0.12)
                ui.keyUp("enter")
                decision = "rejected" if a.scenario == "upstream" else "accepted"
                wait_log("CEF_SMOKE_DIALOG " + decision + " attached=" + attached)
            print("PASS: native top-level and attached dialogs", flush=True)
        if a.scenario in ("all", "file-dialogs"):
            import importlib.util
            spec = importlib.util.spec_from_file_location(
                "file_dialog_checks",
                Path(__file__).with_name("file_dialog_checks.py"))
            fdm = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(fdm)

            fd_dirs = {"open": "open", "open-attached": "open",
                       "cancel-open": "open", "filter": "open",
                       "directory": "dirs", "multiple": "multiple",
                       "save": ".", "save-attached": ".", "overwrite": "."}

            def type_text(text, via_clipboard):
                # The mac runner intermittently truncates ui.write bursts
                # after a couple of characters; the clipboard path bypasses
                # the keyboard pipeline entirely. Confirm the pasteboard
                # actually took the text before trusting cmd+v.
                if via_clipboard and sys.platform == "darwin":
                    subprocess.run(["pbcopy"], input=text.encode(), check=True)
                    board = subprocess.run(["pbpaste"], capture_output=True,
                                           text=True).stdout
                    if board == text:
                        ui.hotkey("command", "a")
                        ui.hotkey("command", "v")
                        return
                    print("type_text: pasteboard mismatch, typing",
                          flush=True)
                ui.write(text, interval=0.05)

            def cdp_set_case(case):
                """Set the case name through the DevTools protocol: the mac
                runner's synthetic keyboard events drop, duplicate and
                reorder characters, so the field is written directly in the
                page (the input event still fires, keeping verification
                honest). The case name is only a parameter carrier here;
                keyboard input itself is covered by the IME scenarios."""
                with urllib.request.urlopen(
                    f"http://127.0.0.1:{debug_port}/json/list", timeout=10
                ) as response:
                    pages = json.load(response)
                page = next(
                    p
                    for p in pages
                    if p.get("type") == "page" and "role=main" in p.get("url", "")
                )
                ws = websocket.create_connection(
                    page["webSocketDebuggerUrl"], suppress_origin=True,
                    timeout=10)
                try:
                    expression = (
                        "(() => { const el = document.getElementById('name');"
                        " el.focus(); el.value = " + json.dumps(case) + ";"
                        " el.dispatchEvent(new Event('input',"
                        " {bubbles: true})); })()"
                    )
                    ws.send(json.dumps({
                        "id": 1,
                        "method": "Runtime.evaluate",
                        "params": {"expression": expression},
                    }))
                    while True:
                        response = json.loads(ws.recv())
                        if response.get("id") == 1:
                            assert "error" not in response, response
                            break
                finally:
                    ws.close()

            def type_case(case):
                """Fill the case name and verify it landed, re-entering via
                the DevTools protocol when keystrokes were dropped or
                mangled."""
                for attempt in range(4):
                    if sys.platform == "darwin":
                        cdp_set_case(case)
                    else:
                        click(*control("main", "name"))
                        ui.hotkey("ctrl", "a")
                        ui.press("backspace")
                        ui.write(case, interval=0.05)
                    for _ in range(2):
                        time.sleep(0.3)
                        # Newline suffix (with \r\n normalised): "open" must
                        # not match a report of "open-attached".
                        marker = "main:input:" + case + "\n"
                        if marker in log.read_text(encoding="utf-8",
                                                   errors="replace"
                                                   ).replace("\r\n", "\n"):
                            return
                    if sys.platform == "darwin":
                        dismiss_mac_alerts()
                        activate_mac_app()
                raise AssertionError(
                    f"input field never received {case!r}")

            def fd_request(case):
                activate_mac_app()
                if sys.platform == "darwin" and bundle_id and case in fd_dirs:
                    # NSOpenPanel/NSSavePanel reopen at their remembered
                    # location and ignore the requested start directory; pin
                    # the panel to the case's single-item fixture view (or
                    # the fixture root for save cases) instead.
                    subprocess.run(
                        ["defaults", "write", bundle_id,
                         "NSNavLastRootDirectory",
                         str((file_dir / fd_dirs[case]).resolve())],
                        check=True)
                # A previous case's panel can linger briefly after its
                # result arrived; let it disappear so fd_opened cannot
                # mistake it for this case's chooser.
                deadline = time.monotonic() + 10
                while fd_is_open() and time.monotonic() < deadline:
                    time.sleep(0.2)
                type_case(case)
                ui.press("f9")

            def fd_opened(case):
                # The native chooser steals focus. On Windows read the
                # foreground window via ctypes and wait until it is no
                # longer one of the app's own windows — save dialogs title
                # themselves "Save As" (locale-dependent), so title equality
                # cannot be used. On macOS the panel runs in our own
                # process, so match any on-screen layer-0 window whose
                # title is not one of the app's own windows.
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    title, rect = "", None
                    if sys.platform == "win32":
                        import ctypes

                        hwnd = ctypes.windll.user32.GetForegroundWindow()
                        for wdw in ui.getAllWindows():
                            if wdw._hWnd == hwnd:
                                title = wdw.title or ""
                                rect = (wdw.left, wdw.top, wdw.width, wdw.height)
                                break
                        if title.startswith("CEF smoke"):
                            rect = None
                    else:
                        import Quartz

                        windows = Quartz.CGWindowListCopyWindowInfo(
                            Quartz.kCGWindowListOptionOnScreenOnly,
                            Quartz.kCGNullWindowID)
                        for w in windows:
                            if w.get(Quartz.kCGWindowOwnerPID) != app.pid:
                                continue
                            # Attached panels are sheets above layer 0, so
                            # only the menu-bar/status levels and the app's
                            # own window titles are excluded.
                            if w.get(Quartz.kCGWindowLayer) in (24, 25):
                                continue
                            title = w.get(Quartz.kCGWindowName) or ""
                            if "CEF smoke" in title:
                                continue
                            b = w[Quartz.kCGWindowBounds]
                            rect = (b["X"], b["Y"], b["Width"], b["Height"])
                            break
                    if rect is not None:
                        screenshot("file-" + case + "-open")
                        return rect
                    time.sleep(.2)
                screenshot("file-" + case + "-not-open")
                raise AssertionError("File dialog for case " + case + " never appeared")

            def fd_key(*args):
                # Modifier chords ("ctrl","a" / "command","shift","g") are
                # pressed and RELEASED here: a keyDown without its keyUp
                # latches the modifier system-wide and turns every later
                # keystroke in subsequent scenarios into a shortcut.
                i = 0
                held = []
                while i < len(args):
                    k = args[i]
                    if k == "click" and i + 2 < len(args):
                        ui.click(int(args[i + 1]), int(args[i + 2]))
                        i += 3
                        continue
                    if k == "shift-click" and i + 2 < len(args):
                        x_c, y_c = int(args[i + 1]), int(args[i + 2])
                        if sys.platform == "darwin":
                            # Combine both delivery paths: hold shift down
                            # (global modifier state) AND bake the shift
                            # flag into the mouse events themselves, with
                            # small gaps so the window server can merge
                            # them — the degraded runner drops rapid
                            # synthetic events.
                            import Quartz

                            ui.keyDown("shift")
                            time.sleep(0.15)
                            for kind in (Quartz.kCGEventLeftMouseDown,
                                         Quartz.kCGEventLeftMouseUp):
                                event = Quartz.CGEventCreateMouseEvent(
                                    None, kind, (x_c, y_c),
                                    Quartz.kCGMouseButtonLeft)
                                Quartz.CGEventSetFlags(
                                    event, Quartz.kCGEventFlagMaskShift)
                                Quartz.CGEventPost(
                                    Quartz.kCGHIDEventTap, event)
                                time.sleep(0.08)
                            time.sleep(0.15)
                            ui.keyUp("shift")
                        else:
                            ui.keyDown("shift")
                            ui.click(x_c, y_c)
                            ui.keyUp("shift")
                        i += 3
                        continue
                    if k == "ctrl-shift-click" and i + 2 < len(args):
                        mod = "ctrl" if sys.platform == "win32" else "command"
                        ui.keyDown(mod)
                        ui.click(int(args[i + 1]), int(args[i + 2]))
                        ui.keyUp(mod)
                        i += 3
                        continue
                    if k == "esc":
                        ui.press("escape")
                    elif k == "enter":
                        ui.press("enter")
                    elif k in ("ctrl", "command", "shift", "alt"):
                        ui.keyDown(k)
                        held.append(k)
                    else:
                        ui.press(k)
                    i += 1
                for k in reversed(held):
                    ui.keyUp(k)

            def fd_paste(text):
                # macOS: type directly into the focused Go-to-sheet field —
                # the runner paths are ASCII and the pbcopy+cmd+v chain
                # silently produced an empty sheet field on runners. The
                # only Unicode paste (中文文件.txt) happens on Windows.
                if sys.platform == "darwin":
                    ui.write(text, interval=0.02)
                    time.sleep(.3)
                    return
                # Unicode-safe clipboard: clip.exe decodes stdin as the
                # ANSI codepage, which mangles non-ASCII names on en-US
                # runners; Set-Clipboard takes proper Unicode.
                quoted = "'" + text.replace("'", "''") + "'"
                subprocess.run(
                    ["powershell", "-NoProfile", "-Command",
                     "Set-Clipboard -Value " + quoted], check=True)
                ui.hotkey("ctrl", "v")
                time.sleep(.4)

            def fd_is_open():
                # Mirrors fd_opened's detection, as a predicate: a chooser
                # owns the foreground (Win32) or a non-app layer-0 window
                # exists (macOS). Save dialogs title themselves "Save As",
                # so title equality cannot be used.
                if sys.platform == "win32":
                    import ctypes

                    hwnd = ctypes.windll.user32.GetForegroundWindow()
                    for wdw in ui.getAllWindows():
                        if wdw._hWnd == hwnd:
                            return not (wdw.title or "").startswith("CEF smoke")
                    return False
                import Quartz

                windows = Quartz.CGWindowListCopyWindowInfo(
                    Quartz.kCGWindowListOptionOnScreenOnly,
                    Quartz.kCGNullWindowID)
                for w in windows:
                    if w.get(Quartz.kCGWindowOwnerPID) != app.pid:
                        continue
                    # Sheets live above layer 0; see fd_opened.
                    if w.get(Quartz.kCGWindowLayer) in (24, 25):
                        continue
                    title = w.get(Quartz.kCGWindowName) or ""
                    if "CEF smoke" in title:
                        continue
                    return True
                return False

            def fd_result(case):
                marker = "CEF_SMOKE_FILE_RESULT "
                deadline = time.monotonic() + 40
                while time.monotonic() < deadline:
                    matches = [
                        ln
                        for ln in log.read_text(encoding="utf-8", errors="replace").splitlines()
                        if ln.startswith(marker)
                        and json.loads(ln[len(marker):])["case"] == case
                    ]
                    if matches:
                        return json.loads(matches[-1][len(marker):])
                    if app.poll() is not None:
                        raise AssertionError("Application exited while waiting for " + case)
                    time.sleep(.2)
                raise AssertionError("No result for file dialog case " + case)

            fdm.check_file_dialogs(
                file_dir, sys.platform, fd_request, fd_opened, fd_key, fd_paste,
                fd_result, screenshot, is_open=fd_is_open)
            print("PASS: native file dialogs (open/save/multiple/directory/filter/cancel/overwrite)", flush=True)
        if a.scenario in ("all", "multiwindow"):
            main_origin = left, top
            click(*control("main", "new"))
            wait_log("secondary:ready")
            time.sleep(1)
            left, top = origin()
            click(*control("secondary", "media"))
            wait_log("secondary:media:NotAllowedError")
            click(*control("secondary", "name"))
            ui.write("SECOND", interval=0.1)
            click(*control("secondary", "greet"))
            wait_log('CEF_SMOKE_GREET "SECOND"')
            ui.hotkey("ctrl", "m")
            wait_log("CEF_SMOKE_EXECJS secondary")
            click(*control("secondary", "close"))
            time.sleep(1)
            assert app.poll() is None, "Closing secondary terminated the application"
            left, top = main_origin
            click(*control("main", "name"))
            ui.hotkey("command" if sys.platform == "darwin" else "ctrl", "a")
            ui.write("MAIN-AGAIN", interval=0.1)
            click(*control("main", "greet"))
            wait_log('CEF_SMOKE_GREET "MAIN-AGAIN"')
            print(
                "PASS: independent windows, shortcuts, permission denial, close and restored focus",
                flush=True,
            )
        if a.scenario in ("all", "mouse"):
            click(*control("main", "overlay"))
            wait_log("overlay:ready")
            time.sleep(1)
            # The overlay is placed inside the main viewport, over its header. Its
            # mouse policy must route a real OS click to the underlying main window.
            # Activate the underlying window first: AppKit otherwise consumes
            # its first click for activation, independently of mouse passthrough.
            click(800, 600)
            tx, ty = left + 120, top + 90
            before = log.read_text(encoding="utf-8", errors="replace").count(
                "main:pointer:"
            )
            ui.click(tx, ty)
            time.sleep(0.5)
            assert (
                log.read_text(encoding="utf-8", errors="replace").count("main:pointer:")
                > before
            )
            click(*control("main", "capture"))
            time.sleep(0.5)
            before = log.read_text(encoding="utf-8", errors="replace").count(
                "overlay:pointer:"
            )
            ui.click(tx, ty)
            time.sleep(0.5)
            assert (
                log.read_text(encoding="utf-8", errors="replace").count(
                    "overlay:pointer:"
                )
                > before
            )
            screenshot("mouse-policy")
            ox, oy = window_position("CEF smoke overlay ready")
            ui.moveTo(tx, ty)
            ui.mouseDown()
            time.sleep(0.2)
            ui.dragTo(tx + 5, ty, duration=0.2, button="left", mouseDownUp=False)
            time.sleep(0.3)
            ui.dragTo(tx + 95, ty + 35, duration=1, button="left", mouseDownUp=False)
            ui.mouseUp()
            time.sleep(0.5)
            nx, ny = window_position("CEF smoke overlay ready")
            assert 75 <= nx - ox <= 110 and 25 <= ny - oy <= 45, (ox, oy, nx, ny)
            print("PASS: dragging opaque frameless content", flush=True)
            click(800, 600)
            click(*control("main", "close-overlay"))
            time.sleep(0.5)
            print(
                "PASS: whole-window mouse passthrough and restored capture", flush=True
            )
        if a.scenario in ("all", "drop"):
            dropped = a.output / "拖放测试.txt"
            dropped.write_text("CEF native file drop probe")
            source_position = a.output / "source-position.json"
            source_log = (a.output / "drop-source.log").open("w")
            source = subprocess.Popen(
                [
                    sys.executable,
                    str(Path(__file__).with_name("native_drop_source.py")),
                    str(dropped),
                    str(source_position),
                ],
                env=env,
                stdout=source_log,
                stderr=subprocess.STDOUT,
            )
            for _ in range(300):
                if source_position.exists():
                    break
                if source.poll() is not None:
                    raise AssertionError(
                        "Native drop source failed; see drop-source.log"
                    )
                time.sleep(0.1)
            assert (
                source_position.exists()
            ), "Native drop source did not become ready; see drop-source.log"
            sx, sy = json.loads(source_position.read_text())
            ui.click(sx, sy)
            time.sleep(0.5)

            def drag_file(x, y):
                ui.moveTo(sx, sy)
                ui.mouseDown()
                time.sleep(0.3)
                ui.dragTo(
                    left + x, top + y, duration=2, button="left", mouseDownUp=False
                )
                time.sleep(0.5)
                ui.mouseUp()
                time.sleep(0.5)

            drag_file(100, 295)
            wait_log("CEF_SMOKE_DROP main")
            text = log.read_text(encoding="utf-8", errors="replace")
            assert "拖放测试.txt" in text and 'ElementID:"drop"' in text
            count = text.count("CEF_SMOKE_DROP")
            drag_file(100, 380)
            assert (
                log.read_text(encoding="utf-8", errors="replace").count(
                    "CEF_SMOKE_DROP"
                )
                == count
            )
            screenshot("file-drop")
            source.terminate()
            source.wait(timeout=10)
            source = None
            print(
                "PASS: native Unicode file drop and rejection outside targets",
                flush=True,
            )
        # AppKit consumes the first click when returning from a native drag
        # source or another window. Activate the host before clicking Quit.
        click(800, 600)
        # Open DevTools, return to the host content and request graceful exit.
        click(*control("main", "quit"))
        wait_log("CEF_SMOKE_QUIT requested")
        for _ in range(3):
            click(*control("main", "tools"))
            time.sleep(1.2)
            if sys.platform == "darwin":
                import Quartz

                windows = Quartz.CGWindowListCopyWindowInfo(
                    Quartz.kCGWindowListOptionOnScreenOnly,
                    Quartz.kCGNullWindowID
                )
                titles = [str(w.get(Quartz.kCGWindowName, "")) for w in windows]
            else:
                titles = ui.getAllTitles()
            if any("DevTools" in title for title in titles):
                break
        screenshot("devtools")
        (a.output / "window-titles.json").write_text(json.dumps(
            list(titles)))
        assert any("DevTools" in title for title in titles), titles
        # A loaded runner can need well over 30s to finish the graceful
        # CEF shutdown with DevTools open; keep the bound generous.
        app.wait(timeout=90)
        assert app.returncode == 0, app.returncode
        text = log.read_text(encoding="utf-8", errors="replace")
        assert (
            "CEF_SMOKE_EXIT" in text
            and "live browsers" not in text
            and "Check failed:" not in text
        )
        print("PASS: native CEF render, keyboard, RPC, ExecJS, resize, shutdown")
    finally:
        if sys.exc_info()[0] is not None:
            screenshot("failure")
        if source is not None and source.poll() is None:
            source.terminate()
            source.wait(timeout=10)
        if sys.platform == "win32" and app.poll() not in (None, 0):
            subprocess.run(
                [
                    "powershell",
                    "-NoProfile",
                    "-Command",
                    "Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000; StartTime=(Get-Date).AddMinutes(-5)} -ErrorAction SilentlyContinue | Format-List TimeCreated,Message",
                ],
                stdout=(a.output / "crash-events.txt").open("w"),
                stderr=subprocess.STDOUT,
                timeout=20,
                check=False,
            )
        if sys.platform == "darwin":
            for report in (Path.home() / "Library/Logs/DiagnosticReports").glob(
                "*CEF*"
            ):
                if report.is_file():
                    shutil.copy2(report, a.output / report.name)
            subprocess.run(
                ["ps", "-axo", "pid,ppid,command"],
                stdout=(a.output / "processes.txt").open("w"),
                check=False,
            )
            if app.poll() is None:
                subprocess.run(
                    [
                        "sample",
                        str(app.pid),
                        "1",
                        "-file",
                        str(a.output / "sample.txt"),
                    ],
                    timeout=15,
                    check=False,
                    capture_output=True,
                )
        if app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=10)
            except subprocess.TimeoutExpired:
                app.kill()

        cef_log = a.runtime / "debug.log"
        if cef_log.exists():
            shutil.copy2(cef_log, a.output / "cef.log")
