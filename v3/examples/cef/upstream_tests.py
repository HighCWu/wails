#!/usr/bin/env python3
"""Run the actual upstream window/dialog tests for the selected native build.

These tests validate shared host contracts, not a running CEF browser. Native
browser coverage lives in smoke.py and native_smoke.py.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--tags', default='')
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
module = Path(__file__).resolve().parents[2]
package = module / 'pkg/application'
metadata = json.loads(subprocess.check_output(
    ['go', 'list', '-json', '-tags', a.tags, './pkg/application'], cwd=module, text=True))
# Keep upstream source files authoritative: new tests in these files are picked
# up automatically. WebView2 process-recovery tests are deliberately separate.
sources = {
    'webview_window_test.go', 'webview_window_options_test.go',
    'webview_window_titlebar_doubleclick_test.go', 'webview_window_linux_test.go',
    'mainthread_darwin_test.go',
}
available = metadata.get('TestGoFiles', []) + metadata.get('XTestGoFiles', [])
selected = sorted(f for f in available if f in sources or (f.startswith('dialogs') and f.endswith('_test.go')))
tests = sorted({name for f in selected for name in re.findall(
    r'func (Test\w+)\(t \*testing\.T\)', (package / f).read_text(encoding="utf-8"))})
if not tests:
    raise SystemExit('No upstream tests discovered')
a.output.mkdir(parents=True, exist_ok=True)
command = ['go', 'test', '-json', '-count=1', '-timeout=5m', '-tags', a.tags,
           './pkg/application', '-run', '^(' + '|'.join(tests) + ')$']
result = subprocess.run(command, cwd=module, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8")
(a.output / 'events.jsonl').write_text(result.stdout, encoding='utf-8')
passed, skipped = set(), set()
for line in result.stdout.splitlines():
    try:
        event = json.loads(line)
    except json.JSONDecodeError:
        print(line)
        continue
    if event.get('Action') == 'pass':
        passed.add(event.get('Test'))
    if event.get('Action') == 'skip':
        skipped.add(event.get('Test'))
    if event.get('Action') == 'output':
        print(event.get('Output', ''), end='')
missing = sorted(set(tests) - passed)
(a.output / 'results.json').write_text(json.dumps(
    {'tags': a.tags, 'sources': selected, 'tests': tests, 'missing': missing,
     'skipped': sorted(x for x in skipped if x)}, indent=2))
if result.returncode or missing or skipped:
    raise SystemExit(f'Upstream parity failed: exit={result.returncode}, missing={missing}, skipped={skipped}')
print(f'PASS: {len(tests)} unmodified upstream window/dialog tests ({a.tags or "default"})')
