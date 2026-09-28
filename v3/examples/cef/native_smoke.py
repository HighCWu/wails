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
import pyautogui as ui

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("binary", type=Path)
p.add_argument("--runtime", required=True, type=Path)
p.add_argument("--output", required=True, type=Path)
p.add_argument("--helper", type=Path)
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
log = a.output / "application.log"


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
        click(260, 223)
        # Wails SetSize is the outer window size on Windows. Account for native
        # frame decorations instead of requiring a 1000x700 browser viewport.
        wait_log("main:size:")
        sizes = re.findall(r"main:size:(\d+)x(\d+)", log.read_text(errors="replace"))
        assert any(
            950 <= int(w) <= 1000 and 650 <= int(h) <= 700 for w, h in sizes
        ), sizes
        screenshot("resized")
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
