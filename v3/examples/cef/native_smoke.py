#!/usr/bin/env python3
"""Native CEF interaction test for disposable Windows/macOS CI desktops."""
import argparse
from collections import Counter
import os
from pathlib import Path
import subprocess
import time
import pyautogui as ui

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('binary',type=Path)
p.add_argument('--runtime',required=True,type=Path)
p.add_argument('--output',required=True,type=Path)
p.add_argument('--helper',type=Path)
a=p.parse_args()
if os.environ.get('GITHUB_ACTIONS')!='true':
    raise SystemExit('Native input automation is restricted to disposable GitHub Actions desktops')
a.output.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,WAILS_WEBVIEW_BACKEND='cef',WAILS_CEF_DIR=str(a.runtime.resolve()),WAILS_CEF_LOG_TO_FILE='1',CEF_SMOKE_DELAY_QUIT='1')
if a.helper: env['WAILS_CEF_SUBPROCESS_PATH']=str(a.helper.resolve())
log=a.output/'application.log'

def wait_log(text,timeout=40):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        if text in log.read_text(errors='replace'): return
        if app.poll() is not None: raise AssertionError(f'Application exited: {app.returncode}')
        time.sleep(.2)
    raise AssertionError('Missing log marker: '+text)

def screenshot(name):
    image=ui.screenshot().convert('RGB');image.save(a.output/(name+'.png'));return image

with log.open('w') as output:
    app=subprocess.Popen([str(a.binary.resolve())],env=env,stdout=output,stderr=subprocess.STDOUT)
    try:
        wait_log('main:ready')
        for _ in range(60):
            image=screenshot('initial')
            if Counter(image.getdata())[(30,102,245)]>50000:break
            time.sleep(.5)
        else: raise AssertionError('CEF content was not presented')
        pixels=image.load()
        points=[(x,y) for y in range(image.height) for x in range(image.width) if pixels[x,y]==(30,102,245)]
        scale=image.width/ui.size().width
        left=min(x for x,y in points)/scale;top=min(y for x,y in points)/scale
        def click(x,y):ui.click(left+x,top+y);time.sleep(.3)
        click(100,164);ui.write('CEF154',interval=.1);click(290,164)
        wait_log('CEF_SMOKE_GREET "CEF154"')
        time.sleep(1)
        assert Counter(screenshot('rpc').getdata())[(0,255,0)]>200000
        ui.keyDown('ctrl');time.sleep(.15);ui.press('m');ui.keyUp('ctrl')
        wait_log('CEF_SMOKE_EXECJS main')
        click(260,223);wait_log('main:size:1000x700')
        screenshot('resized')
        # Open DevTools, return to the host content and request graceful exit.
        click(334,223);click(438,164);time.sleep(2);screenshot('devtools')
        app.wait(timeout=30)
        assert app.returncode==0, app.returncode
        text=log.read_text(errors='replace')
        assert 'CEF_SMOKE_EXIT' in text and 'live browsers' not in text and 'Check failed:' not in text
        print('PASS: native CEF render, keyboard, RPC, ExecJS, resize, shutdown')
    finally:
        if app.poll() is None:
            app.terminate()
            try:app.wait(timeout=10)
            except subprocess.TimeoutExpired:app.kill()
