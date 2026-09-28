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
parser.add_argument("--extended", action="store_true")
parser.add_argument("--ime", action="store_true")
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
if args.extended:
    env["CEF_SMOKE_FRAMELESS"] = "1"
    env["WAILS_CEF_SWITCHES"] = "use-fake-device-for-media-stream"
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
    if args.backend == "system" or args.extended:
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
    if args.ime:
        env["XDG_CONFIG_HOME"] = str(out / "config")
        env["XDG_RUNTIME_DIR"] = str(out / "runtime")
        Path(env["XDG_RUNTIME_DIR"]).mkdir(mode=0o700, exist_ok=True)
        bus = start(["dbus-daemon", "--session", "--nofork", "--print-address=1"], "dbus", stdout=subprocess.PIPE)
        env["DBUS_SESSION_BUS_ADDRESS"] = bus.stdout.readline().decode().strip()
        env["GTK_IM_MODULE"] = "ibus"
        env["XMODIFIERS"] = "@im=ibus"
        socket = out / "ibus.socket"
        socket.unlink(missing_ok=True)
        env["IBUS_ADDRESS"] = "unix:path=" + str(socket)
        start(["ibus-daemon", "--single", "--xim", "--address=" + env["IBUS_ADDRESS"],
               "--panel=disable", "--config=disable", "--emoji-extension=disable"], "ibus", stdout=subprocess.DEVNULL)
        time.sleep(1)
        engine_log = open(out / "ime-engine-output.log", "w")
        logs.append(engine_log)
        start(["/usr/bin/python3", str(Path(__file__).with_name("ime_engine.py"))], "ime-engine", stdout=engine_log)
        time.sleep(1)
        run("ibus", "engine", "cef-smoke")
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
        result = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "^" + re.escape(args.binary.name) + "$" if args.extended else "^CEF smoke (ready|main)$"],
                                env=env, capture_output=True, text=True, timeout=5)
        if result.returncode == 0:
            window = result.stdout.splitlines()[0]
            break
        if app.poll() is not None:
            raise RuntimeError("Application exited before runtime was ready")
        time.sleep(.1)
    if not window:
        (out / "not-ready-tree.txt").write_text(run("xwininfo", "-root", "-tree"))
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
    for _ in range(400):
        if "main:ready" in (out / "application.log").read_text():
            break
        time.sleep(.1)
    assert "main:ready" in (out / "application.log").read_text(), "Runtime readiness timed out"
    colors = screenshot("initial")
    assert colors[(30, 102, 245)] > 50000 and colors[(255, 255, 255)] > 200000
    run("xdotool", "windowactivate", "--sync", window)
    click(100, 164)
    time.sleep(.3)
    run("xdotool", "type", "--clearmodifiers", "CEF154")
    screenshot("typed")
    click(290, 164)
    wait_title(window, "CEF smoke RPC OK")
    assert screenshot("rpc")[(0, 255, 0)] > 200000
    assert 'CEF_SMOKE_GREET "CEF154"' in (out / "application.log").read_text()
    run("xdotool", "windowactivate", "--sync", window)
    (out / "focus.txt").write_text(run("xdotool", "getwindowfocus") + "\n" + run("xdotool", "getactivewindow"))
    # GTK's upstream key debounce includes modifier keypresses (50 ms).
    run("xdotool", "keydown", "ctrl")
    time.sleep(.12)
    run("xdotool", "key", "m")
    run("xdotool", "keyup", "ctrl")
    wait_title(window, "CEF smoke ExecJS OK")
    assert "CEF_SMOKE_EXECJS" in (out / "application.log").read_text()
    if args.ime:
        click(100, 164)
        run("xdotool", "key", "ctrl+a", "BackSpace", "F6")
        time.sleep(.4)
        screenshot("ime-preedit")
        run("xdotool", "key", "space")
        time.sleep(.3)
        click(290, 164)
        time.sleep(.5)
        log = (out / "application.log").read_text()
        assert 'CEF_SMOKE_GREET "中文输入"' in log
        assert "compositionstart:" in log and "compositionend:中文输入" in log
        print("PASS: IBus Chinese preedit, composition events, commit and RPC", flush=True)
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
    if args.extended:
        def wait_log(expected):
            for _ in range(100):
                if expected in (out / "application.log").read_text():
                    return
                time.sleep(.1)
            raise AssertionError("Missing log: " + expected)

        # Native frameless drag through the runtime bridge.
        run("xdotool", "mousemove", str(x + 200), str(y + 60), "mousedown", "1")
        time.sleep(.2)
        run("xdotool", "mousemove", str(x + 205), str(y + 60))
        time.sleep(.3)
        run("xdotool", "mousemove", str(x + 355), str(y + 110))
        time.sleep(.4)
        run("xdotool", "mouseup", "1")
        geometry = run("xwininfo", "-id", window)
        nx = int(re.search(r"Absolute upper-left X:\s+(-?\d+)", geometry)[1])
        ny = int(re.search(r"Absolute upper-left Y:\s+(-?\d+)", geometry)[1])
        assert abs(nx - x - 150) <= 6 and abs(ny - y - 50) < 5, (x, y, nx, ny)
        x, y = nx, ny
        print("PASS: frameless drag", flush=True)
        run("xdotool", "mousemove", str(x+998), str(y+698))
        time.sleep(.2)
        run("xdotool", "mousedown", "1")
        time.sleep(.1)
        run("xdotool", "mousemove", str(x+1000), str(y+700))
        time.sleep(.2)
        run("xdotool", "mousemove", str(x+1030), str(y+720))
        time.sleep(.4)
        run("xdotool", "mouseup", "1")
        geometry = run("xwininfo", "-id", window)
        assert int(re.search(r"Width:\s+(\d+)", geometry)[1]) >= 1025
        assert int(re.search(r"Height:\s+(\d+)", geometry)[1]) >= 715
        run("xdotool", "windowsize", window, "1000", "700")
        print("PASS: frameless edge resize", flush=True)
        click(65, 223)
        wait_log("main:media:allowed:audio,video")
        print("PASS: media allow with fake devices (real permission handler)", flush=True)
        # Clipboard path checks Unicode separately from composition/IME.
        subprocess.run(["xclip", "-selection", "clipboard"], input="中文输入验证".encode(), env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        click(100, 164)
        run("xdotool", "key", "ctrl+a", "ctrl+v")
        time.sleep(.3)
        click(290, 164)
        wait_log('CEF_SMOKE_GREET "中文输入验证"')
        print("PASS: Unicode clipboard and RPC", flush=True)
        click(205, 223)
        second = None
        for _ in range(100):
            result = subprocess.run(["xdotool", "search", "--name", "^CEF smoke secondary ready$"],
                                    env=env, text=True, capture_output=True)
            if result.returncode == 0:
                second = result.stdout.splitlines()[0]
                break
            time.sleep(.1)
        assert second, "Second window did not become ready"
        run("xdotool", "windowmove", second, "30", "30", "windowactivate", "--sync", second)
        geometry = run("xwininfo", "-id", second)
        sx = int(re.search(r"Absolute upper-left X:\s+(-?\d+)", geometry)[1])
        sy = int(re.search(r"Absolute upper-left Y:\s+(-?\d+)", geometry)[1])
        run("xdotool", "mousemove", str(sx + 65), str(sy + 223), "click", "1")
        wait_log("secondary:media:NotAllowedError")
        run("xdotool", "mousemove", str(sx + 100), str(sy + 164), "click", "1", "type", "SECOND")
        run("xdotool", "mousemove", str(sx + 290), str(sy + 164), "click", "1")
        wait_log('CEF_SMOKE_GREET "SECOND"')
        run("xdotool", "windowactivate", "--sync", second, "key", "alt+F4")
        time.sleep(.5)
        assert app.poll() is None, "Closing secondary terminated the app"
        run("xdotool", "windowactivate", "--sync", window)
        print("PASS: second window RPC, permission deny, independent close", flush=True)
        ids = re.findall(r"\b0x[0-9a-f]+\b", run("xwininfo", "-id", window, "-tree"))
        (out / "dnd-properties.txt").write_text("\n".join(i + " " + run("xprop", "-id", i, "XdndAware", "XdndProxy") for i in dict.fromkeys(ids)))
        dropped = out / "拖放测试.txt"
        dropped.write_text("CEF native file drop probe")
        start(["/usr/bin/python3", str(Path(__file__).with_name("drop_source.py")), str(dropped)],
              "drop-source", stdout=subprocess.DEVNULL)
        time.sleep(.6)
        source = run("xdotool", "search", "--name", "^CEF smoke file source$").splitlines()[0]
        run("xdotool", "windowmove", source, "1100", "5", "windowactivate", "--sync", source)
        geometry = run("xwininfo", "-id", source)
        dx = int(re.search(r"Absolute upper-left X:\s+(-?\d+)", geometry)[1]) + 60
        dy = int(re.search(r"Absolute upper-left Y:\s+(-?\d+)", geometry)[1]) + 40
        def drag_file(tx, ty):
            run("xdotool", "windowactivate", "--sync", source)
            run("xdotool", "mousemove", str(dx), str(dy), "mousedown", "1")
            for step in range(1, 21):
                run("xdotool", "mousemove", str(int(dx + (tx-dx)*step/20)),
                    str(int(dy + (ty-dy)*step/20)))
                time.sleep(.05)
            time.sleep(.3)
            run("xdotool", "mouseup", "1")
            time.sleep(.4)
        drag_file(x+100, y+295)
        screenshot("file-drop")
        wait_log("CEF_SMOKE_DROP main")
        assert str(dropped) in (out / "application.log").read_text()
        assert 'ElementID:"drop"' in (out / "application.log").read_text()
        print("PASS: native file drop with Unicode path and DOM target details", flush=True)
        drop_count = (out / "application.log").read_text().count("CEF_SMOKE_DROP")
        drag_file(x+100, y+470)
        assert (out / "application.log").read_text().count("CEF_SMOKE_DROP") == drop_count
        run("xdotool", "windowactivate", "--sync", window)
        click(205, 223)
        time.sleep(1)
        second = run("xdotool", "search", "--name", "^CEF smoke secondary ready$").splitlines()[0]
        run("xdotool", "windowmove", second, "30", "30")
        geometry = run("xwininfo", "-id", second)
        sx = int(re.search(r"Absolute upper-left X:\s+(-?\d+)", geometry)[1])
        sy = int(re.search(r"Absolute upper-left Y:\s+(-?\d+)", geometry)[1])
        drag_file(sx+100, sy+295)
        assert (out / "application.log").read_text().count("CEF_SMOKE_DROP") == drop_count
        run("xdotool", "windowactivate", "--sync", second, "key", "alt+F4")
        print("PASS: drops outside targets and into disabled windows are ignored", flush=True)
        run("xdotool", "windowminimize", source)
        screenshot("extended")
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
    assert app.wait(timeout=15) == 0
    final_log = (out / "application.log").read_text()
    assert "CEF_SMOKE_EXIT" in final_log
    assert "live browsers" not in final_log and "Check failed:" not in final_log
    print("PASS:", args.backend, "render, keyboard, RPC, ExecJS, resize, shutdown;", out)
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
