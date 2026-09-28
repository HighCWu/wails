#!/usr/bin/env python3
"""Read-only presentation probe on the selected DISPLAY; never sends input.

The example opts out of keyboard focus and exits itself after 15 seconds.
"""
import argparse
import os, subprocess, time, sys, re
from pathlib import Path
from collections import Counter
from PIL import Image
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--runtime", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ, CEF_SMOKE_PASSIVE='1',WAILS_WEBVIEW_BACKEND='cef',WAILS_CEF_DIR=str(args.runtime.resolve()),GTK_IM_MODULE='gtk-im-context-simple',XMODIFIERS='@im=none',XDG_CACHE_HOME=str(out/'cache'))
def read(*args): return subprocess.check_output(args,env=env,text=True,timeout=8).strip()
log=open(out/'application.log','w')
app=subprocess.Popen([str(args.binary.resolve())],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
try:
 window=None
 for _ in range(100):
  found=subprocess.run(['xdotool','search','--onlyvisible','--name','^CEF smoke (main|ready)$'],env=env,text=True,capture_output=True)
  if found.returncode==0: window=found.stdout.splitlines()[0];break
  time.sleep(.1)
 assert window
 hints=read('xprop','-id',window,'WM_HINTS');(out/'hints.txt').write_text(hints)
 assert 'input or input focus: False' in hints,hints
 print(hints,flush=True)
 for _ in range(100):
  if 'main:passive:green' in (out/'application.log').read_text(): break
  time.sleep(.1)
 assert 'main:passive:green' in (out/'application.log').read_text()
 time.sleep(.5)
 read('import','-window',window,str(out/'window.png'))
 with Image.open(out/'window.png') as image:
  rgb=image.convert('RGB')
  pixels=Counter(zip(*[iter(rgb.tobytes())]*3))
 print('BLUE',pixels[(30,102,245)],'GREEN',pixels[(0,255,0)],flush=True)
 (out/'window-tree.txt').write_text(read('xwininfo','-id',window,'-tree'))
 assert pixels[(30,102,245)]>50000 and pixels[(0,255,0)]>200000
 assert app.wait(timeout=20)==0
 final_log=(out/'application.log').read_text()
 assert 'live browsers' not in final_log and 'Check failed:' not in final_log
 print('PASS: passive native presentation, RPC, shutdown; no input or focus requests',flush=True)
finally:
 if app.poll() is None: app.terminate();app.wait(timeout=8)
 log.close()
