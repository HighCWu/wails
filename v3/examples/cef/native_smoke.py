#!/usr/bin/env python3
"""Native CEF interaction test for disposable Windows/macOS CI desktops."""
import argparse
from collections import Counter
import os
from pathlib import Path
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
env = dict(
    os.environ,
    WAILS_WEBVIEW_BACKEND="cef",
    WAILS_CEF_DIR=str(a.runtime.resolve()),
    WAILS_CEF_LOG_TO_FILE="1",
    CEF_SMOKE_DELAY_QUIT="1",
)
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
log = a.output / "application.log"
source = None


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
        re.escape(role) + r":controls:(\{[^\n]+\})", log.read_text(errors="replace")
    )
    return json.loads(matches[-1])[name]


def window_position(title):
    if sys.platform == "darwin":
        import Quartz

        windows = Quartz.CGWindowListCopyWindowInfo(
            Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID
        )
        for w in windows:
            if w.get(Quartz.kCGWindowName) == title:
                bounds = w[Quartz.kCGWindowBounds]
                return bounds["X"], bounds["Y"]
    else:
        windows = ui.getWindowsWithTitle(title)
        if windows:
            return windows[0].left, windows[0].top
    raise AssertionError("Missing native window " + title)


def wait_log(text, timeout=40):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if text in log.read_text(errors="replace"):
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
    app = subprocess.Popen(
        [str(a.binary.resolve())], env=env, stdout=output, stderr=subprocess.STDOUT
    )
    try:
        wait_log("main:ready")
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

        click(100, 164)
        ui.write("CEF154", interval=0.1)
        click(290, 164)
        wait_log('CEF_SMOKE_GREET "CEF154"')
        time.sleep(1)
        assert Counter(screenshot("rpc").getdata())[(0, 255, 0)] > 200000
        ui.keyDown("ctrl")
        time.sleep(0.15)
        ui.press("m")
        ui.keyUp("ctrl")
        wait_log("CEF_SMOKE_EXECJS main")
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
        sizes = re.findall(r"main:size:(\d+)x(\d+)", log.read_text(errors="replace"))
        assert any(
            950 <= int(w) <= 1000 and 650 <= int(h) <= 700 for w, h in sizes
        ), sizes
        screenshot("resized")
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
        for name, attached in [("dialog", "false"), ("attached", "true")]:
            click(*control("main", name))
            wait_log("CEF_SMOKE_DIALOG opened attached=" + attached)
            time.sleep(1)
            screenshot(name)
            ui.press("enter")
            wait_log("CEF_SMOKE_DIALOG accepted attached=" + attached)
        print("PASS: native top-level and attached dialogs", flush=True)
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
        click(*control("main", "overlay"))
        wait_log("overlay:ready")
        time.sleep(1)
        # The overlay is placed inside the main viewport, over its header. Its
        # mouse policy must route a real OS click to the underlying main window.
        tx, ty = left + 120, top + 90
        before = log.read_text(errors="replace").count("main:pointer:")
        ui.click(tx, ty)
        time.sleep(0.5)
        assert log.read_text(errors="replace").count("main:pointer:") > before
        click(*control("main", "capture"))
        time.sleep(0.5)
        before = log.read_text(errors="replace").count("overlay:pointer:")
        ui.click(tx, ty)
        time.sleep(0.5)
        assert log.read_text(errors="replace").count("overlay:pointer:") > before
        screenshot("mouse-policy")
        ox, oy = window_position("CEF smoke overlay ready")
        ui.moveTo(tx, ty)
        ui.mouseDown()
        time.sleep(0.2)
        ui.moveTo(tx + 5, ty, duration=0.2)
        time.sleep(0.3)
        ui.moveTo(tx + 95, ty + 35, duration=1)
        ui.mouseUp()
        time.sleep(0.5)
        nx, ny = window_position("CEF smoke overlay ready")
        assert 75 <= nx - ox <= 110 and 25 <= ny - oy <= 45, (ox, oy, nx, ny)
        print("PASS: dragging opaque frameless content", flush=True)
        click(*control("main", "close-overlay"))
        time.sleep(0.5)
        print("PASS: whole-window mouse passthrough and restored capture", flush=True)
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
        for _ in range(100):
            if source_position.exists():
                break
            if source.poll() is not None:
                raise AssertionError("Native drop source failed; see drop-source.log")
            time.sleep(0.1)
        sx, sy = json.loads(source_position.read_text())

        def drag_file(x, y):
            ui.moveTo(sx, sy)
            ui.mouseDown()
            time.sleep(0.3)
            ui.moveTo(left + x, top + y, duration=2)
            time.sleep(0.5)
            ui.mouseUp()
            time.sleep(0.5)

        drag_file(100, 295)
        wait_log("CEF_SMOKE_DROP main")
        text = log.read_text(errors="replace")
        assert "拖放测试.txt" in text and 'ElementID:"drop"' in text
        count = text.count("CEF_SMOKE_DROP")
        drag_file(100, 380)
        assert log.read_text(errors="replace").count("CEF_SMOKE_DROP") == count
        screenshot("file-drop")
        source.terminate()
        source.wait(timeout=10)
        source = None
        print(
            "PASS: native Unicode file drop and rejection outside targets", flush=True
        )
        # Open DevTools, return to the host content and request graceful exit.
        click(334, 223)
        click(438, 164)
        time.sleep(2)
        screenshot("devtools")
        if sys.platform == "darwin":
            import Quartz

            windows = Quartz.CGWindowListCopyWindowInfo(
                Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID
            )
            titles = [str(w.get(Quartz.kCGWindowName, "")) for w in windows]
        else:
            titles = ui.getAllTitles()
        (a.output / "window-titles.json").write_text(json.dumps(titles))
        assert any("DevTools" in title for title in titles), titles
        app.wait(timeout=30)
        assert app.returncode == 0, app.returncode
        text = log.read_text(errors="replace")
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
