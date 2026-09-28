"""Shared assertions for real GTK, Win32 and Cocoa file-picker interaction."""
import time
from pathlib import Path

CASES = ['open', 'open-attached', 'cancel-open', 'save', 'save-attached',
         'cancel-save', 'multiple', 'directory', 'filter', 'overwrite']


def prepare_files(root):
    root.mkdir(parents=True, exist_ok=True)
    (root / 'multiple').mkdir(exist_ok=True)
    (root / '目录').mkdir(exist_ok=True)
    for name in ['中文文件.txt', 'existing.txt', 'excluded.bin',
                 'multiple/first.txt', 'multiple/第二个.txt']:
        (root / name).write_text('CEF file dialog fixture\n', encoding='utf-8')


def check_file_dialogs(root, platform, request, opened, key, paste, result, screenshot):
    """Adapters provide physical input and native dialog geometry, never results."""
    root = root.resolve()
    for case in CASES:
        request(case)
        rect = opened(case)
        screenshot('file-' + case + '-open')
        if case.startswith('cancel'):
            key('esc')
            answer = result(case)
            assert answer['paths'] == [], answer
            error = answer['error'].lower()
            assert not error or 'cancel' in error or '800704c7' in error, answer
        else:
            expected = []
            if case == 'multiple':
                expected = [root / 'multiple/first.txt', root / 'multiple/第二个.txt']
                if platform == 'win32':
                    key('alt', 'n')
                    paste(' '.join('"' + str(p) + '"' for p in expected))
                elif platform == 'darwin':
                    # Exactly two selectable fixture files: click the first
                    # row, then extend to the last row.
                    x, y, w, h = rect
                    key('click', x + w * .5, y + h * .21)
                    time.sleep(.3)
                    key('command', 'shift', 'down')
                else:
                    # GTK list: click the first file row, then shift+Down
                    # extends the selection to the next (last) row. Window-
                    # relative coordinates inside this dialog are unstable;
                    # row-height arithmetic repeatedly misfired, so the
                    # runner's click adapter also accepts a row offset: click
                    # at the measured first-row Y from the rect, then a
                    # keyboard-only extension which cannot miss.
                    x, y, w, h = rect
                    key('click', x + w * .5, y + h * .128)
                    time.sleep(.5)
                    # Ctrl+click the second row: GTK multi-select toggle.
                    # Keyboard extensions (End/shift+Home) do not reach the
                    # list because the chooser keeps keyboard focus on its
                    # location bar after programmatic activation.
                    key('ctrl-shift-click', x + w * .5, y + h * .155)
            else:
                filename = {'save':'saved.txt', 'save-attached':'attached.txt',
                            'overwrite':'existing.txt', 'directory':'目录'}.get(case, '中文文件.txt')
                expected = [root / filename]
                if platform == 'darwin':
                    key('command', 'shift', 'g')
                    time.sleep(.3)
                    paste(str(expected[0]))
                    key('enter')
                    time.sleep(.6)
                elif platform == 'win32':
                    key('alt', 'n')
                    paste(str(expected[0]))
                else:
                    key('ctrl', 'l')
                    paste(str(expected[0]))
            key('enter')
            if case == 'overwrite':
                # NSSavePanel/IFileSaveDialog/GtkFileChooser ask before replacing
                # an existing file. The dialog itself never writes file contents.
                time.sleep(.8)
                screenshot('file-overwrite-confirmation')
                key('left')
                key('enter')
            answer = result(case)
            assert not answer['error'], answer
            # GTK save dialogs append the active filter extension when the
            # typed name does not already end with it (saved.txt -> saved.txt.txt).
            def matches(actual, want):
                actual = Path(actual).resolve()
                if actual == want.resolve():
                    return True
                return (platform == 'linux' and want.suffix == '.txt'
                        and actual.name == want.name + '.txt'
                        and actual.parent == want.parent)
            got = sorted(Path(p).resolve() for p in answer['paths'])
            assert got == sorted(Path(w).resolve() for w in expected) or (
                len(got) == len(expected)
                and all(any(matches(a, w) for a in got) for w in expected)
                and all(any(matches(a, w) for w in expected) for a in got)
            ), answer
        screenshot('file-' + case + '-closed')
        print('PASS: native file dialog ' + case, flush=True)
    assert (root / 'existing.txt').read_text(encoding='utf-8') == 'CEF file dialog fixture\n'
