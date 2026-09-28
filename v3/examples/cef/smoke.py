#!/usr/bin/env python3
"""Exercise the example in a private Xvfb display; never sends input to DISPLAY.

Requires Xvfb, openbox, xdotool, ImageMagick import, and Python Pillow.
"""
import argparse
from collections import Counter
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import tempfile
import time

from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--runtime", type=Path)
parser.add_argument("--backend", choices=["cef", "system"], default="cef")
parser.add_argument("--output", type=Path)
parser.add_argument("--devtools", action="store_true")
args = parser.parse_args()
if args.backend == "cef" and not args.runtime:
    parser.error("--runtime is required for CEF")
out = args.output or Path(tempfile.mkdtemp(prefix="wails-cef-smoke-"))
out.mkdir(parents=True, exist_ok=True)
out = out.resolve()
processes = []
logs = []
env = dict(os.environ, WAILS_WEBVIEW_BACKEND=args.backend,
           GTK_IM_MODULE="gtk-im-context-simple", XMODIFIERS="@im=none",
           XDG_CACHE_HOME=str(out / "cache"))
env.pop("WAILS_CEF_SWITCHES", None)
if args.runtime:
    env["WAILS_CEF_DIR"] = str(args.runtime.resolve())


def start(command, name, **kwargs):
    log = open(out / (name + ".log"), "w")
    logs.append(log)
    process = subprocess.Popen(command, env=env, stderr=log,
                               start_new_session=True, **kwargs)
    processes.append(process)
    return process


def run(*command):
    return subprocess.check_output(command, env=env, text=True, timeout=10).strip()


def wait_title(window, expected):
    if args.backend == "system":
        time.sleep(1)
        return
    for _ in range(100):
        if run("xdotool", "getwindowname", window) == expected:
            return
        time.sleep(.1)
    raise AssertionError("Window title did not become " + expected)


def screenshot(name):
    time.sleep(.4)  # Title changes can precede compositor presentation.
    filename = out / (name + ".png")
    run("import", "-window", "root", str(filename))
    with Image.open(filename) as image:
        rgb = image.convert("RGB")
        pixels = Counter(zip(*[iter(rgb.tobytes())] * 3))
    print(name, {str(c): pixels[c] for c in
                 [(255, 255, 255), (30, 102, 245), (0, 255, 0)]}, flush=True)
    return pixels


try:
    xvfb = start(["Xvfb", "-displayfd", "1", "-screen", "0", "1280x900x24",
                  "-nolisten", "tcp"], "xvfb", stdout=subprocess.PIPE)
    if not select.select([xvfb.stdout], [], [], 10)[0]:
        raise RuntimeError("Xvfb startup timed out")
    display = xvfb.stdout.readline().decode().strip()
    if not display.isdigit():
        raise RuntimeError("Xvfb did not allocate a display")
    env["DISPLAY"] = ":" + display
    start(["openbox"], "window-manager", stdout=subprocess.DEVNULL)
    time.sleep(.5)
    app_log = open(out / "application.log", "w")
    logs.append(app_log)
    app = subprocess.Popen([str(args.binary.resolve())], env=env,
                           stdout=app_log, stderr=subprocess.STDOUT,
                           start_new_session=True)
    processes.append(app)
    window = None
    for _ in range(200):
        result = subprocess.run(["xdotool", "search", "--name", "^CEF smoke ready$" if args.backend == "cef" else "^CEF smoke$"],
                                env=env, capture_output=True, text=True, timeout=5)
        if result.returncode == 0:
            window = result.stdout.splitlines()[0]
            break
        if app.poll() is not None:
            raise RuntimeError("Application exited before runtime was ready")
        time.sleep(.1)
    if not window:
        screenshot("not-ready")
        raise AssertionError("Runtime never became ready; see application.log")
    run("xdotool", "windowmove", window, "80", "80")
    time.sleep(.3)
    geometry = run("xwininfo", "-id", window)
    x = int(re.search(r"Absolute upper-left X:\s+(-?\d+)", geometry)[1])
    y = int(re.search(r"Absolute upper-left Y:\s+(-?\d+)", geometry)[1])

    def click(dx, dy):
        run("xdotool", "mousemove", str(x + dx), str(y + dy), "click", "1")

    if args.backend == "system":
        time.sleep(2)
    colors = screenshot("initial")
    assert colors[(30, 102, 245)] > 50000 and colors[(255, 255, 255)] > 200000
    click(100, 164)
    time.sleep(.3)
    run("xdotool", "type", "--clearmodifiers", "CEF154")
    screenshot("typed")
    click(290, 164)
    wait_title(window, "CEF smoke RPC OK")
    assert screenshot("rpc")[(0, 255, 0)] > 200000
    assert 'CEF_SMOKE_GREET "CEF154"' in (out / "application.log").read_text()
    if args.backend == "cef":
        run("xdotool", "key", "ctrl+m")
        wait_title(window, "CEF smoke ExecJS OK")
        assert "CEF_SMOKE_EXECJS" in (out / "application.log").read_text()
    else:
        print("SKIP: system Ctrl+M/ExecJS (shortcut did not fire in this environment)")
    run("xdotool", "windowsize", window, "1000", "700")
    time.sleep(.5)
    assert screenshot("resized")[(0, 255, 0)] > 500000
    tree = run("xwininfo", "-id", window, "-tree")
    (out / "window-tree.txt").write_text(tree)
    geometry = run("xwininfo", "-id", window)
    assert re.search(r"Width:\s+1000\b", geometry)
    assert re.search(r"Height:\s+700\b", geometry)
    if args.backend == "cef":
        assert "1000x700" in tree
    if args.devtools:
        click(438, 164)
        for _ in range(50):
            result = subprocess.run(["xdotool", "search", "--name", "DevTools"],
                                    env=env, capture_output=True, text=True, timeout=5)
            if result.returncode == 0:
                break
            time.sleep(.1)
        assert result.returncode == 0, "DevTools window did not open"
        screenshot("devtools")
    run("xdotool", "windowactivate", "--sync", window, "key", "alt+F4")
    assert app.wait(timeout=12) == 0
    assert "CEF_SMOKE_EXIT" in (out / "application.log").read_text()
    print("PASS:", args.backend, "render, keyboard, RPC, resize, shutdown;", out)
finally:
    for process in reversed(processes):
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
    for log in logs:
        log.close()
